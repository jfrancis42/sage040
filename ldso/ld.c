/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ld.c - the dynamic linker, /lib/ld.so.
 *
 * Reference: System V ABI, "Dynamic Linking" (the generic chapters), and
 * the Motorola 68000 processor supplement for the relocation types.
 *
 * A program linked against a shared library carries a PT_INTERP naming
 * this file. The kernel loads the program and this, and starts here.
 * What happens then is the textbook sequence and nothing more:
 *
 *   1. find the program's .dynamic, through the auxiliary vector;
 *   2. load every DT_NEEDED library, breadth first, each at whatever
 *      address mmap gives it -- LD_LIBRARY_PATH, then /lib;
 *   3. relocate the libraries, then the program, resolving every
 *      symbol NOW (no lazy binding: see below);
 *   4. run each library's initialisers, the dependencies first;
 *   5. jump to the program's entry point with the stack untouched.
 *
 * NO LAZY BINDING. Lazy binding resolves a function on its first call,
 * through a trampoline that saves every register, looks the name up
 * and patches the GOT -- which makes startup cheaper and every other
 * thing about the linker harder: the trampoline is assembly with its
 * own ABI, a missing symbol turns into a crash in the middle of a run
 * instead of a refusal at the start, and the saving is a few hundred
 * lookups. Linux itself defaults programs to eager binding when they
 * are built with -z now. Here every program is.
 *
 * Symbol lookup is the classic ELF rule: the program first, then the
 * libraries in the order they were loaded, first definition wins. So a
 * program that defines malloc gets its malloc used by the C library as
 * well, exactly as on any other Unix.
 *
 * The libraries' text is mapped read-only from the file, and the kernel
 * shares those pages between every process that maps the same file
 * (kernel/textcache.c). That -- not the saving on disk -- is the point
 * of the exercise.
 *
 * This file is freestanding: no C library, because the C library is
 * one of the things it loads. Everything it needs is below or in
 * start.s, which also makes the system calls.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            s32;

extern long dl_syscall(long nr, long a1, long a2, long a3, long a4,
                       long a5, long a6);

/* --- the kernel's interface (Linux/m68k's) ------------------------- */

#define SYS_exit        1
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_lseek       19
#define SYS_munmap      91
#define SYS_mprotect    125
#define SYS_mmap2       192
#define SYS_getuid32    199
#define SYS_getgid32    200
#define SYS_geteuid32   201
#define SYS_getegid32   202

#define PROT_NONE       0
#define PROT_READ       1
#define PROT_WRITE      2
#define PROT_EXEC       4
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

#define PAGE            4096u
#define PAGE_DOWN(x)    ((x) & ~(PAGE - 1))
#define PAGE_UP(x)      (((x) + PAGE - 1) & ~(PAGE - 1))

/* --- ELF32 ----------------------------------------------------------- */

struct ehdr {
    u8  ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

struct dyn {
    s32 tag;
    u32 val;
};

struct sym {
    u32 name, value, size;
    u8  info, other;
    u16 shndx;
};

struct rela {
    u32 offset, info;
    s32 addend;
};

#define ET_DYN          3
#define EM_68K          4
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PF_X            1
#define PF_W            2
#define PF_R            4

#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_INIT         12
#define DT_TEXTREL      22
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27
#define DT_FLAGS        30
#define DF_TEXTREL      0x4

#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STB_WEAK        2
#define STT_FUNC        2
#define SHN_UNDEF       0

/* The 68000 supplement's numbers, and every one a linker emits for
 * position-independent code on this target. */
#define R_68K_NONE      0
#define R_68K_32        1
#define R_68K_PC32      4
#define R_68K_COPY      19
#define R_68K_GLOB_DAT  20
#define R_68K_JMP_SLOT  21
#define R_68K_RELATIVE  22

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHNUM        5
#define AT_ENTRY        9

/* --- small freestanding helpers ------------------------------------ */

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void mcopy(void *d, const void *s, u32 n)
{
    u8 *dp = d;
    const u8 *sp = s;

    while (n--) {
        *dp++ = *sp++;
    }
}

static void mzero(void *d, u32 n)
{
    u8 *dp = d;

    while (n--) {
        *dp++ = 0;
    }
}

static void say(const char *s)
{
    dl_syscall(SYS_write, 2, (long)s, (long)slen(s), 0, 0, 0);
}

static void out(const char *s)
{
    dl_syscall(SYS_write, 1, (long)s, (long)slen(s), 0, 0, 0);
}

static void out_hex(u32 v)
{
    char b[11];
    int i;

    b[0] = '0';
    b[1] = 'x';
    for (i = 0; i < 8; i++) {
        b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    }
    b[10] = '\0';
    out(b);
}

static void fail(const char *what, const char *name)
{
    say("ld.so: ");
    say(what);
    if (name) {
        say(": ");
        say(name);
    }
    say("\n");
    dl_syscall(SYS_exit, 127, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

static int is_err(long r)
{
    return (unsigned long)r >= (unsigned long)-4095;
}

/* --- the objects ----------------------------------------------------- */

#define MAX_OBJS    16
#define MAX_PHDRS   12

struct obj {
    char path[128];
    u32 base;                   /* 0 for the program: it is linked where it is */
    struct dyn *dyn;
    const char *strtab;
    struct sym *symtab;
    u32 *hash;
    struct rela *rela;
    u32 relasz;
    struct rela *jmprel;
    u32 pltrelsz;
    u32 init;
    u32 init_array;
    u32 init_arraysz;
    int textrel;
    struct phdr ph[MAX_PHDRS];
    int phnum;
};

static struct obj objs[MAX_OBJS];
static int nobjs;
static char **envp;

/*
 * IS THIS PROGRAM PRIVILEGED? Linux answers this with AT_SECURE in the
 * auxiliary vector, set by the kernel at exec; here the same question
 * is asked directly, because uid != euid is exactly what a set-user-id
 * exec leaves behind and there is nothing else that sets it.
 *
 * It matters because of LD_LIBRARY_PATH. A set-user-id program that
 * took its library search path from whoever ran it would load a libc
 * of their choosing AS ROOT, which is a root shell for anybody who can
 * write a directory -- the oldest hole in dynamic linking.
 *
 * Nothing on this disk is both set-user-id and dynamic today: su, sudo
 * and passwd are static, and auth/Makefile fails the build if one of
 * them comes out with a PT_INTERP. But that invariant lives in one
 * Makefile, and any `fsimg put -m 4755` anywhere would escape it. The
 * loader is where the check belongs, because the loader is what the
 * variable talks to.
 */
static int privileged(void)
{
    static int known;           /* 0 unasked, 1 no, 2 yes */

    if (!known) {
        known = (dl_syscall(SYS_getuid32, 0, 0, 0, 0, 0, 0) !=
                     dl_syscall(SYS_geteuid32, 0, 0, 0, 0, 0, 0) ||
                 dl_syscall(SYS_getgid32, 0, 0, 0, 0, 0, 0) !=
                     dl_syscall(SYS_getegid32, 0, 0, 0, 0, 0, 0)) ? 2 : 1;
    }
    return known == 2;
}

static const char *getenv_(const char *name)
{
    u32 n = slen(name);
    char **e;

    for (e = envp; *e; e++) {
        const char *s = *e;
        u32 i;

        for (i = 0; i < n && s[i] == name[i]; i++) {
        }
        if (i == n && s[n] == '=') {
            return s + n + 1;
        }
    }
    return 0;
}

/* Everything .dynamic says, turned into addresses in this process. */
static void parse_dynamic(struct obj *o)
{
    struct dyn *d;

    for (d = o->dyn; d->tag != DT_NULL; d++) {
        u32 v = d->val;

        switch (d->tag) {
        case DT_HASH:         o->hash = (u32 *)(o->base + v); break;
        case DT_STRTAB:       o->strtab = (const char *)(o->base + v); break;
        case DT_SYMTAB:       o->symtab = (struct sym *)(o->base + v); break;
        case DT_RELA:         o->rela = (struct rela *)(o->base + v); break;
        case DT_RELASZ:       o->relasz = v; break;
        case DT_JMPREL:       o->jmprel = (struct rela *)(o->base + v); break;
        case DT_PLTRELSZ:     o->pltrelsz = v; break;
        case DT_INIT:         o->init = o->base + v; break;
        case DT_INIT_ARRAY:   o->init_array = o->base + v; break;
        case DT_INIT_ARRAYSZ: o->init_arraysz = v; break;
        case DT_TEXTREL:      o->textrel = 1; break;
        case DT_FLAGS:
            if (v & DF_TEXTREL) {
                o->textrel = 1;
            }
            break;
        default:
            break;
        }
    }
}

/* --- loading a library -------------------------------------------- */

static int read_at(int fd, u32 off, void *buf, u32 len)
{
    if (is_err(dl_syscall(SYS_lseek, fd, (long)off, 0, 0, 0, 0))) {
        return -1;
    }
    return dl_syscall(SYS_read, fd, (long)buf, (long)len, 0, 0, 0) == (long)len
           ? 0 : -1;
}

static int prot_of(u32 flags)
{
    return ((flags & PF_R) ? PROT_READ : 0) |
           ((flags & PF_W) ? PROT_WRITE : 0) |
           ((flags & PF_X) ? PROT_EXEC : 0);
}

/*
 * Map a shared object's segments. First the whole span is reserved
 * with one anonymous mapping, which is how the object gets an address;
 * then each segment is mapped over its part of that with MAP_FIXED, and
 * whatever the segments do not cover is given back.
 *
 * A text segment is mapped from the file READ-ONLY, and that is the
 * mapping the kernel shares between processes. A data segment is
 * mapped from the file too, privately and writable, and the rest of
 * its last page past the file's bytes is cleared -- those bytes are
 * whatever followed the segment in the file, often the section headers,
 * and they are .bss.
 */
static void load_object(struct obj *o, int fd)
{
    struct ehdr eh;
    u32 lo = 0xffffffffu, hi = 0, covered = 0;
    long r;
    int i;

    if (read_at(fd, 0, &eh, sizeof(eh)) < 0 ||
        eh.ident[0] != 0x7f || eh.ident[1] != 'E' || eh.ident[2] != 'L' ||
        eh.ident[3] != 'F' || eh.ident[4] != 1 || eh.ident[5] != 2 ||
        eh.machine != EM_68K || eh.type != ET_DYN ||
        eh.phentsize != sizeof(struct phdr)) {
        fail("not a 68k shared object", o->path);
    }
    if (eh.phnum > MAX_PHDRS) {
        fail("too many program headers", o->path);
    }
    o->phnum = eh.phnum;
    if (read_at(fd, eh.phoff, o->ph, eh.phnum * sizeof(struct phdr)) < 0) {
        fail("cannot read program headers", o->path);
    }

    for (i = 0; i < o->phnum; i++) {
        struct phdr *p = &o->ph[i];

        if (p->type != PT_LOAD) {
            continue;
        }
        if (PAGE_DOWN(p->vaddr) < lo) {
            lo = PAGE_DOWN(p->vaddr);
        }
        if (PAGE_UP(p->vaddr + p->memsz) > hi) {
            hi = PAGE_UP(p->vaddr + p->memsz);
        }
        if ((p->vaddr - p->offset) & (PAGE - 1)) {
            fail("segment not congruent with its file offset", o->path);
        }
    }
    if (hi <= lo) {
        fail("no loadable segments", o->path);
    }

    r = dl_syscall(SYS_mmap2, 0, (long)(hi - lo), PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (is_err(r)) {
        fail("out of memory mapping", o->path);
    }
    o->base = (u32)r - lo;

    for (i = 0; i < o->phnum; i++) {
        struct phdr *p = &o->ph[i];
        u32 start, fend, mapend, memend;
        int prot;

        if (p->type == PT_DYNAMIC) {
            o->dyn = (struct dyn *)(o->base + p->vaddr);
        }
        if (p->type != PT_LOAD) {
            continue;
        }
        prot = prot_of(p->flags);
        start = o->base + PAGE_DOWN(p->vaddr);
        fend = o->base + p->vaddr + p->filesz;
        mapend = PAGE_UP(fend);
        memend = PAGE_UP(o->base + p->vaddr + p->memsz);

        if (p->filesz) {
            r = dl_syscall(SYS_mmap2, (long)start, (long)(mapend - start),
                           prot, MAP_PRIVATE | MAP_FIXED, fd,
                           (long)(PAGE_DOWN(p->offset) / PAGE));
            if (is_err(r)) {
                fail("cannot map a segment", o->path);
            }
            if ((p->flags & PF_W) && mapend > fend) {
                mzero((void *)fend, mapend - fend);
            }
        } else {
            mapend = start;
        }
        if (memend > mapend) {
            r = dl_syscall(SYS_mmap2, (long)mapend, (long)(memend - mapend),
                           prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
                           -1, 0);
            if (is_err(r)) {
                fail("cannot map .bss", o->path);
            }
        }
        /* The reservation between the last segment and this one. */
        if (covered && start > covered) {
            dl_syscall(SYS_munmap, (long)covered, (long)(start - covered),
                       0, 0, 0, 0);
        }
        covered = memend;
    }
    if (!o->dyn) {
        fail("no dynamic section", o->path);
    }
    parse_dynamic(o);
}

static int try_open(struct obj *o, const char *dir, u32 dlen, const char *name)
{
    u32 n = slen(name);
    long fd;

    if (dlen + 1 + n + 1 > sizeof(o->path)) {
        return -1;
    }
    mcopy(o->path, dir, dlen);
    o->path[dlen] = '/';
    mcopy(o->path + dlen + 1, name, n + 1);
    fd = dl_syscall(SYS_open, (long)o->path, 0, 0, 0, 0, 0);
    return is_err(fd) ? -1 : (int)fd;
}

/* LD_LIBRARY_PATH's directories in order, then /lib. A name with a
 * slash in it is a path and is used as it stands. */
static int find_library(struct obj *o, const char *name)
{
    const char *lp = privileged() ? 0 : getenv_("LD_LIBRARY_PATH");
    int fd;

    if (lp) {
        while (*lp) {
            const char *end = lp;

            while (*end && *end != ':') {
                end++;
            }
            if (end > lp && (fd = try_open(o, lp, (u32)(end - lp), name)) >= 0) {
                return fd;
            }
            lp = *end ? end + 1 : end;
        }
    }
    for (fd = 0; name[fd]; fd++) {
        if (name[fd] == '/') {
            long r;

            if (slen(name) + 1 > sizeof(o->path)) {
                return -1;
            }
            mcopy(o->path, name, slen(name) + 1);
            r = dl_syscall(SYS_open, (long)o->path, 0, 0, 0, 0, 0);
            return is_err(r) ? -1 : (int)r;
        }
    }
    return try_open(o, "/lib", 4, name);
}

/* Breadth first, each library once, in the order the ELF rules give
 * for symbol lookup. */
static void load_needed(void)
{
    int i;

    for (i = 0; i < nobjs; i++) {
        struct dyn *d;

        for (d = objs[i].dyn; d->tag != DT_NULL; d++) {
            const char *name;
            struct obj *o;
            int j, fd, dup = 0;

            if (d->tag != DT_NEEDED) {
                continue;
            }
            name = objs[i].strtab + d->val;
            /* Already loaded under this name? Compared by the last
             * component of the path it was found at. */
            for (j = 1; j < nobjs; j++) {
                const char *p = objs[j].path, *base = p;

                for (; *p; p++) {
                    if (*p == '/') {
                        base = p + 1;
                    }
                }
                if (seq(base, name)) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (nobjs == MAX_OBJS) {
                fail("too many libraries", name);
            }
            o = &objs[nobjs];
            fd = find_library(o, name);
            if (fd < 0) {
                fail("cannot find library", name);
            }
            load_object(o, fd);
            dl_syscall(SYS_close, fd, 0, 0, 0, 0, 0);
            nobjs++;
        }
    }
}

/* --- symbols ------------------------------------------------------- */

static u32 elf_hash(const char *n)
{
    u32 h = 0, g;

    while (*n) {
        h = (h << 4) + (u8)*n++;
        g = h & 0xf0000000u;
        if (g) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}

/*
 * `name` defined in `o`, through its hash table. `plt_ok` accepts the
 * program's own undefined-but-addressed functions: when a program takes
 * the address of printf, the linker makes printf's PLT entry THE address
 * of printf for the whole process, and a library that compares against
 * it has to be given the same one. Only a JMP_SLOT must not see it --
 * that would bind the call to itself.
 */
static struct sym *lookup_in(struct obj *o, const char *name, u32 h,
                             int plt_ok)
{
    u32 nb, *bucket, *chain, i;

    if (!o->hash || !o->symtab) {
        return 0;
    }
    nb = o->hash[0];
    bucket = o->hash + 2;
    chain = bucket + nb;
    for (i = bucket[h % nb]; i; i = chain[i]) {
        struct sym *s = &o->symtab[i];
        u32 bind = s->info >> 4;

        if (bind != STB_GLOBAL && bind != STB_WEAK) {
            continue;
        }
        if (s->shndx == SHN_UNDEF &&
            !(plt_ok && o == &objs[0] && s->value &&
              (s->info & 15) == STT_FUNC)) {
            continue;
        }
        if (seq(o->strtab + s->name, name)) {
            return s;
        }
    }
    return 0;
}

static struct sym *find_symbol(const char *name, int from, int plt_ok,
                               struct obj **where)
{
    u32 h = elf_hash(name);
    int i;

    for (i = from; i < nobjs; i++) {
        struct sym *s = lookup_in(&objs[i], name, h, plt_ok);

        if (s) {
            *where = &objs[i];
            return s;
        }
    }
    return 0;
}

/* --- relocation ---------------------------------------------------- */

static void relocate_table(struct obj *o, struct rela *r, u32 bytes)
{
    u32 n = bytes / sizeof(struct rela), k;

    for (k = 0; k < n; k++, r++) {
        u32 type = r->info & 0xff;
        u32 si = r->info >> 8;
        u32 *where = (u32 *)(o->base + r->offset);
        u32 S = 0;
        struct sym *ls = 0, *ds = 0;
        struct obj *def = 0;

        if (type == R_68K_NONE) {
            continue;
        }
        if (si) {
            const char *name;

            ls = &o->symtab[si];
            name = o->strtab + ls->name;
            if ((ls->info >> 4) == STB_LOCAL) {
                S = o->base + ls->value;
            } else {
                /* A COPY is the program taking a library's variable
                 * into itself; the original is found by looking past
                 * the program. */
                ds = find_symbol(name, type == R_68K_COPY ? 1 : 0,
                                 type != R_68K_JMP_SLOT && type != R_68K_COPY,
                                 &def);
                if (ds) {
                    S = def->base + ds->value;
                } else if ((ls->info >> 4) != STB_WEAK) {
                    fail("undefined symbol", name);
                }
            }
        }

        switch (type) {
        case R_68K_32:
        case R_68K_GLOB_DAT:
            *where = S + (u32)r->addend;
            break;
        case R_68K_JMP_SLOT:
            *where = S;
            break;
        case R_68K_PC32:
            *where = S + (u32)r->addend - (u32)where;
            break;
        case R_68K_RELATIVE:
            *where = o->base + (u32)r->addend;
            break;
        case R_68K_COPY:
            if (ds) {
                mcopy(where, (void *)S, ls->size < ds->size ? ls->size
                                                            : ds->size);
            }
            break;
        default:
            fail("unsupported relocation type in", o->path);
        }
    }
}

/* Text relocations mean writing to pages that are meant to be read-only
 * and shared; they are allowed, by making the segment writable for the
 * duration -- which gives this process its own copy of those pages. A
 * library built with -fPIC has none. */
static void set_text_writable(struct obj *o, int writable)
{
    int i;

    for (i = 0; i < o->phnum; i++) {
        struct phdr *p = &o->ph[i];
        u32 start;

        if (p->type != PT_LOAD || (p->flags & PF_W)) {
            continue;
        }
        start = o->base + PAGE_DOWN(p->vaddr);
        dl_syscall(SYS_mprotect, (long)start,
                   (long)(PAGE_UP(o->base + p->vaddr + p->memsz) - start),
                   prot_of(p->flags) | (writable ? PROT_WRITE : 0), 0, 0, 0);
    }
}

static void relocate(struct obj *o)
{
    if (o->textrel) {
        set_text_writable(o, 1);
    }
    if (o->rela) {
        relocate_table(o, o->rela, o->relasz);
    }
    if (o->jmprel) {
        relocate_table(o, o->jmprel, o->pltrelsz);
    }
    if (o->textrel) {
        set_text_writable(o, 0);
    }
}

/* --- the start ------------------------------------------------------- */

u32 dl_main(u32 *sp)
{
    char **e;
    u32 *aux;
    struct phdr *ph = 0;
    u32 phnum = 0, entry = 0, i;
    int k;

    envp = (char **)sp[3];
    for (e = envp; *e; e++) {
    }
    for (aux = (u32 *)(e + 1); aux[0] != AT_NULL; aux += 2) {
        switch (aux[0]) {
        case AT_PHDR:  ph = (struct phdr *)aux[1]; break;
        case AT_PHNUM: phnum = aux[1]; break;
        case AT_ENTRY: entry = aux[1]; break;
        default: break;
        }
    }
    if (!ph || !entry) {
        fail("no auxiliary vector: run a program, not ld.so", 0);
    }

    /* The program: already loaded, at the addresses it was linked for. */
    objs[0].base = 0;
    mcopy(objs[0].path, "(program)", 10);
    for (i = 0; i < phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) {
            objs[0].dyn = (struct dyn *)ph[i].vaddr;
        }
    }
    if (!objs[0].dyn) {
        fail("the program has no dynamic section", 0);
    }
    parse_dynamic(&objs[0]);
    nobjs = 1;

    load_needed();

    /* ldd, as glibc spells it: list what was loaded, and stop. */
    if (!privileged() && getenv_("LD_TRACE_LOADED_OBJECTS")) {
        for (k = 1; k < nobjs; k++) {
            const char *p = objs[k].path, *base = p;

            for (; *p; p++) {
                if (*p == '/') {
                    base = p + 1;
                }
            }
            out("\t");
            out(base);
            out(" => ");
            out(objs[k].path);
            out(" (");
            out_hex(objs[k].base);
            out(")\n");
        }
        dl_syscall(SYS_exit, 0, 0, 0, 0, 0, 0);
    }

    /*
     * The libraries in reverse, so each one's own data is final before
     * anything loaded ahead of it can COPY from it; the program last,
     * because its COPY relocations take the libraries' initialised
     * variables.
     */
    for (k = nobjs - 1; k >= 1; k--) {
        relocate(&objs[k]);
    }
    relocate(&objs[0]);

    /* Initialisers, dependencies first. The program's own are its
     * crt0's business, as they are in a static program. */
    for (k = nobjs - 1; k >= 1; k--) {
        struct obj *o = &objs[k];

        if (o->init) {
            ((void (*)(void))o->init)();
        }
        for (i = 0; i < o->init_arraysz / 4; i++) {
            void (*fn)(void) = ((void (**)(void))o->init_array)[i];

            if (fn && fn != (void (*)(void))-1) {
                fn();
            }
        }
    }
    return entry;
}
