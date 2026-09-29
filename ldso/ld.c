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
 * THREAD-LOCAL STORAGE. Every object with a PT_TLS segment is a TLS
 * module, numbered by its place in objs[] plus one (the program is 1).
 * Those present at start are laid out in one static block per thread,
 * the program's first (see sage040-dl.h for the layout, which is
 * Linux/m68k's because the linker writes offsets against it), and the
 * first thread's block is made here before any initialiser runs. The
 * C library makes every later thread's from the same layout.
 *
 * AFTER START, it stays: the C library reaches dlopen, dlsym, dladdr,
 * dl_iterate_phdr and the TLS of a library loaded late through the
 * table at the end of this file, whose address it is given in
 * __sage040_dl. A failure inside one of those comes back as an error
 * rather than ending the process -- the only difference between loading
 * a library now and loading one at start.
 *
 * This file is freestanding: no C library, because the C library is
 * one of the things it loads. Everything it needs is below or in
 * start.s, which also makes the system calls.
 */

#include "../libc/picolibc/libos/linux/machine/m68k/sage040-dl.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;   /* as sage040-dl.h and the target have it */
typedef int            s32;

extern long dl_syscall(long nr, long a1, long a2, long a3, long a4,
                       long a5, long a6);
extern int dl_setjmp(unsigned long buf[13]);
extern void dl_longjmp(unsigned long buf[13], int val)
    __attribute__((noreturn));

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
#define SYS_get_thread_area 333
#define SYS_set_thread_area 334

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
#define PT_TLS          7
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
#define STT_TLS         6
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
#define R_68K_TLS_DTPMOD32  40
#define R_68K_TLS_DTPREL32  41
#define R_68K_TLS_TPREL32   42

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHNUM        5
#define AT_ENTRY        9

#define RTLD_NOLOAD     4

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

/*
 * NOT INLINED, AND NOT FOR TIDINESS. Inlined into a loop that also
 * computes the destination from the source, gcc 15.2 for m68k emitted
 * `move.b (%a0)+,(0,%a0,%d0.l)` -- a post-incremented source and a
 * destination indexed off THE SAME register. The 68040 (and QEMU)
 * increment a0 before working out the destination, so every byte went
 * one place too far: a thread's TLS arrived shifted by a byte, and the
 * last byte landed on the next thing in memory. Out of line, with its
 * own two pointers, it is `move.b (%a0)+,(%a1)+`. ports/gcc/patches/03
 * fixes the compiler; this stays, because ld.so is the one program that
 * cannot be rebuilt around a bad compiler after the fact.
 */
static __attribute__((noinline)) void mcopy(void *d, const void *s, u32 n)
{
    u8 *dp = d;
    const u8 *sp = s;

    while (n--) {
        *dp++ = *sp++;
    }
}

static __attribute__((noinline)) void mzero(void *d, u32 n)
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

/*
 * Set while dlopen() is working: a failure then goes back there with
 * its message in dl_err, instead of ending the process as it must while
 * the program is still starting.
 */
static unsigned long *recover;
static char dl_err[200];

static void err_add(u32 *n, const char *s)
{
    while (*s && *n + 1 < sizeof(dl_err)) {
        dl_err[(*n)++] = *s++;
    }
    dl_err[*n] = '\0';
}

static void fail(const char *what, const char *name)
{
    u32 n = 0;

    err_add(&n, what);
    if (name) {
        err_add(&n, ": ");
        err_add(&n, name);
    }
    if (recover) {
        dl_longjmp(recover, 1);
    }
    say("ld.so: ");
    say(dl_err);
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

#define MAX_OBJS    DL_MAX_OBJS
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
    const struct phdr *phmem;   /* the headers as mapped, for dl_iterate_phdr */
    u32 lo, hi;                 /* what it occupies, for dladdr and undoing */
    int refs;                   /* dlopen()s not yet dlclose()d */
    /* Its PT_TLS, if it has one. */
    const void *tls_image;
    u32 tls_filesz, tls_memsz, tls_align;
    u32 tls_offset;             /* from the TCB, when tls_static */
    int tls_static;             /* in the per-thread block, not made late */
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
    o->lo = (u32)r;
    o->hi = (u32)r + (hi - lo);

    for (i = 0; i < o->phnum; i++) {
        struct phdr *p = &o->ph[i];
        u32 start, fend, mapend, memend;
        int prot;

        if (p->type == PT_DYNAMIC) {
            o->dyn = (struct dyn *)(o->base + p->vaddr);
        }
        if (p->type == PT_TLS) {
            o->tls_image = (const void *)(o->base + p->vaddr);
            o->tls_filesz = p->filesz;
            o->tls_memsz = p->memsz;
            o->tls_align = p->align ? p->align : 1;
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
    o->phmem = o->ph;
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
        u32 S = 0, V = 0;       /* its address; its raw st_value, for TLS */
        struct sym *ls = 0, *ds = 0;
        struct obj *def = o;    /* a TLS reloc with no symbol: this one */

        if (type == R_68K_NONE) {
            continue;
        }
        if (si) {
            const char *name;

            ls = &o->symtab[si];
            name = o->strtab + ls->name;
            if ((ls->info >> 4) == STB_LOCAL) {
                S = o->base + ls->value;
                V = ls->value;
            } else {
                /* A COPY is the program taking a library's variable
                 * into itself; the original is found by looking past
                 * the program. */
                ds = find_symbol(name, type == R_68K_COPY ? 1 : 0,
                                 type != R_68K_JMP_SLOT && type != R_68K_COPY,
                                 &def);
                if (ds) {
                    S = def->base + ds->value;
                    V = ds->value;
                } else if ((ls->info >> 4) != STB_WEAK) {
                    fail("undefined symbol", name);
                } else {
                    def = o;
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
        /*
         * Thread-local storage. A TLS symbol's st_value is its offset in
         * its module's block, not an address, so these take V; and the
         * offsets are Linux/m68k's, biased as binutils biases them (see
         * sage040-dl.h).
         */
        case R_68K_TLS_DTPMOD32:
            *where = (u32)(def - objs) + 1;
            break;
        case R_68K_TLS_DTPREL32:
            *where = V + (u32)r->addend - DL_TLS_DTV_OFFSET;
            break;
        case R_68K_TLS_TPREL32:
            /* Initial-exec: a fixed distance from the thread pointer,
             * which only a module in the static block has. A library
             * loaded after start with such an access cannot be given
             * one -- glibc keeps some spare static space for this;
             * here it is refused, rather than handing out an offset
             * into something else. */
            if (!def->tls_static) {
                fail("initial-exec TLS in a library loaded after start",
                     def->path);
            }
            *where = def->tls_offset + V + (u32)r->addend - DL_TLS_TP_OFFSET;
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

/* --- thread-local storage ------------------------------------------ */

static struct dl_tls_layout layout;

static u32 align_up(u32 v, u32 a)
{
    return (v + a - 1) & ~(a - 1);
}

/*
 * The static block: every TLS module present at start, the program's
 * first and at offset 0 -- which is what the linker assumed when it
 * wrote the program's local-exec offsets -- each at its own alignment.
 */
static void tls_layout(void)
{
    u32 off = 0, align = 4;
    int k;

    for (k = 0; k < nobjs; k++) {
        struct obj *o = &objs[k];
        struct dl_tls_module *m;

        if (!o->tls_memsz) {
            continue;
        }
        if (o->tls_align & (o->tls_align - 1)) {
            fail("TLS alignment is not a power of two", o->path);
        }
        off = align_up(off, o->tls_align);
        o->tls_offset = off;
        o->tls_static = 1;
        m = &layout.mod[layout.n++];
        m->modid = (u32)k + 1;
        m->offset = off;
        m->filesz = o->tls_filesz;
        m->memsz = o->tls_memsz;
        m->image = o->tls_image;
        off += o->tls_memsz;
        if (o->tls_align > align) {
            align = o->tls_align;
        }
    }
    layout.size = off;
    layout.align = align;
}

/*
 * The first thread's block, made the same way the C library makes every
 * later thread's (tls.c) -- one mapping holding the TCB, the static
 * block and the DTV -- and handed to the kernel as the thread pointer.
 * Made for every program, TLS or none, so that the C library can count
 * on there being a TCB.
 */
static void tls_first_thread(void)
{
    u32 total = 8 + layout.align + layout.size + DL_DTV_SLOTS * 4 + 4;
    long r = dl_syscall(SYS_mmap2, 0, (long)PAGE_UP(total),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    u32 tcb, *dtv, i;

    if (is_err(r)) {
        fail("no memory for thread-local storage", 0);
    }
    tcb = align_up((u32)r + 8, layout.align);
    dtv = (u32 *)align_up(tcb + layout.size, 4);
    dtv[0] = DL_DTV_SLOTS - 1;
    for (i = 0; i < layout.n; i++) {
        const struct dl_tls_module *m = &layout.mod[i];

        mcopy((void *)(tcb + m->offset), m->image, m->filesz);
        dtv[m->modid] = tcb + m->offset;        /* the rest is zero */
    }
    ((struct dl_tcbhead *)(tcb - 8))->dtv = (unsigned long *)dtv;
    dl_syscall(SYS_set_thread_area, (long)(tcb + DL_TLS_TP_OFFSET),
               0, 0, 0, 0, 0);
}

static u32 *my_dtv(void)
{
    u32 tp = (u32)dl_syscall(SYS_get_thread_area, 0, 0, 0, 0, 0, 0);

    return tp ? (u32 *)((struct dl_tcbhead *)(tp - DL_TLS_TP_OFFSET - 8))->dtv
              : 0;
}

/*
 * A module loaded after start has no place in the static block, so each
 * thread's copy of it is made the first time that thread asks -- which
 * is what __tls_get_addr does, through here, when it finds the DTV slot
 * empty. Mapped by itself, a page at a time; tls_release gives these
 * back when the thread ends.
 */
static void *iface_tls_block(unsigned long *dtv, unsigned long modid)
{
    struct obj *o;
    long r;

    if (!dtv || modid == 0 || modid >= DL_DTV_SLOTS ||
        (int)modid > nobjs) {
        return 0;
    }
    o = &objs[modid - 1];
    if (!o->tls_memsz) {
        return 0;
    }
    if (dtv[modid]) {
        return (void *)dtv[modid];
    }
    r = dl_syscall(SYS_mmap2, 0, (long)PAGE_UP(o->tls_memsz),
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    if (is_err(r)) {
        return 0;
    }
    mcopy((void *)r, o->tls_image, o->tls_filesz);
    dtv[modid] = (u32)r;
    return (void *)r;
}

static void iface_tls_release(unsigned long *dtv)
{
    int m;

    if (!dtv) {
        return;
    }
    for (m = 1; m < DL_DTV_SLOTS && m <= nobjs; m++) {
        struct obj *o = &objs[m - 1];

        if (dtv[m] && o->tls_memsz && !o->tls_static) {
            dl_syscall(SYS_munmap, (long)dtv[m], (long)PAGE_UP(o->tls_memsz),
                       0, 0, 0, 0);
            dtv[m] = 0;
        }
    }
}

/* --- loading after start: dlopen and the rest ----------------------- */

static void run_inits(int from, int to)
{
    int k;
    u32 i;

    for (k = to - 1; k >= from; k--) {
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
}

static const char *base_name(const char *p)
{
    const char *b = p;

    for (; *p; p++) {
        if (*p == '/') {
            b = p + 1;
        }
    }
    return b;
}

static struct obj *loaded(const char *file)
{
    int k;

    for (k = 1; k < nobjs; k++) {
        if (seq(objs[k].path, file) || seq(base_name(objs[k].path), file)) {
            return &objs[k];
        }
    }
    return 0;
}

static int valid_handle(void *h)
{
    struct obj *o = h;

    return o >= objs && o < objs + nobjs;
}

/*
 * dlopen. Everything the start does, for one library and whatever it
 * needs that is not already here: load, lay out its TLS (dynamically:
 * it has no place in the static block), relocate against every object
 * loaded so far, run its initialisers. On any failure the new objects
 * are unmapped and forgotten, and the call says why.
 *
 * Every library is visible to every later lookup -- RTLD_GLOBAL, always
 * -- and none is ever unloaded: dlclose counts, and leaves the library
 * where it is, as musl does. Unloading is where most of the complexity
 * and most of the bugs of a dynamic linker live, and nothing here needs
 * the address space back.
 */
static void *iface_dlopen(const char *file, int mode, const char **err)
{
    unsigned long jb[13];
    int first = nobjs, k;
    struct obj *o;

    *err = 0;
    if (!file) {
        return &objs[0];
    }
    o = loaded(file);
    if (o) {
        o->refs++;
        return o;
    }
    if (mode & RTLD_NOLOAD) {
        *err = "not loaded";
        return 0;
    }
    if (dl_setjmp(jb)) {
        /* Undo: nothing of a half-loaded library may stay findable. */
        recover = 0;
        for (k = first; k < nobjs; k++) {
            if (objs[k].hi > objs[k].lo) {
                dl_syscall(SYS_munmap, (long)objs[k].lo,
                           (long)(objs[k].hi - objs[k].lo), 0, 0, 0, 0);
            }
            mzero(&objs[k], sizeof(objs[k]));
        }
        nobjs = first;
        *err = dl_err;
        return 0;
    }
    recover = jb;
    if (nobjs == MAX_OBJS) {
        fail("too many libraries", file);
    }
    o = &objs[nobjs];
    mzero(o, sizeof(*o));
    k = find_library(o, file);
    if (k < 0) {
        fail("cannot find library", file);
    }
    load_object(o, k);
    dl_syscall(SYS_close, k, 0, 0, 0, 0, 0);
    nobjs++;
    load_needed();
    for (k = nobjs - 1; k >= first; k--) {
        relocate(&objs[k]);
    }
    recover = 0;
    run_inits(first, nobjs);
    objs[first].refs = 1;
    return &objs[first];
}

static struct obj *object_at(const void *addr)
{
    u32 a = (u32)addr;
    int k;

    for (k = 0; k < nobjs; k++) {
        if (a >= objs[k].lo && a < objs[k].hi) {
            return &objs[k];
        }
    }
    return 0;
}

/* What a symbol means to the calling thread: an address, or for a TLS
 * variable its own copy's. */
static void *sym_address(struct obj *o, struct sym *s)
{
    if ((s->info & 15) == STT_TLS) {
        u32 *dtv = my_dtv();
        u32 m = (u32)(o - objs) + 1;
        void *b;

        if (!dtv) {
            return 0;
        }
        b = dtv[m] ? (void *)dtv[m] : iface_tls_block(dtv, m);
        return b ? (u8 *)b + s->value : 0;
    }
    return (void *)(o->base + s->value);
}

/*
 * The objects `h` brought in, breadth first: itself, then what it names
 * as DT_NEEDED, and so on -- the scope dlsym searches for a handle.
 */
static struct sym *lookup_scope(struct obj *h, const char *name,
                                struct obj **where)
{
    struct obj *q[MAX_OBJS];
    int n = 0, i, j;
    u32 hsh = elf_hash(name);

    q[n++] = h;
    for (i = 0; i < n; i++) {
        struct sym *s = lookup_in(q[i], name, hsh, 0);
        struct dyn *d;

        if (s) {
            *where = q[i];
            return s;
        }
        for (d = q[i]->dyn; d && d->tag != DT_NULL; d++) {
            struct obj *dep;

            if (d->tag != DT_NEEDED ||
                !(dep = loaded(q[i]->strtab + d->val))) {
                continue;
            }
            for (j = 0; j < n && q[j] != dep; j++) {
            }
            if (j == n && n < MAX_OBJS) {
                q[n++] = dep;
            }
        }
    }
    return 0;
}

#define HANDLE_DEFAULT  ((void *)0)
#define HANDLE_NEXT     ((void *)-1)

static void *iface_dlsym(void *handle, const char *name, const void *caller,
                         const char **err)
{
    struct obj *where = 0;
    struct sym *s = 0;

    *err = 0;
    /*
     * Through the global scope, a function the program takes the
     * address of answers with the program's PLT entry -- the address the
     * whole process already uses for it (see lookup_in) -- so that
     * dlsym(RTLD_DEFAULT, "f") == &f in the program, as glibc has it.
     */
    if (handle == HANDLE_DEFAULT) {
        s = find_symbol(name, 0, 1, &where);
    } else if (handle == HANDLE_NEXT) {
        struct obj *c = object_at(caller);

        s = c ? find_symbol(name, (int)(c - objs) + 1, 1, &where) : 0;
    } else if (valid_handle(handle)) {
        s = lookup_scope(handle, name, &where);
    } else {
        *err = "invalid handle";
        return 0;
    }
    if (!s) {
        u32 n = 0;

        err_add(&n, "undefined symbol: ");
        err_add(&n, name);
        *err = dl_err;
        return 0;
    }
    return sym_address(where, s);
}

static int iface_dlclose(void *handle, const char **err)
{
    struct obj *o = handle;

    *err = 0;
    if (!valid_handle(handle)) {
        *err = "invalid handle";
        return -1;
    }
    if (o->refs > 0) {
        o->refs--;              /* and it stays loaded: see dlopen */
    }
    return 0;
}

/* The object an address is in, and the nearest symbol at or below it. */
static int iface_dladdr(const void *addr, struct dl_info_ *info)
{
    struct obj *o = object_at(addr);
    struct sym *best = 0;
    u32 a = (u32)addr, i, n;

    if (!o) {
        return 0;
    }
    info->dli_fname = o->path;
    info->dli_fbase = (void *)o->lo;
    info->dli_sname = 0;
    info->dli_saddr = 0;
    n = o->hash ? o->hash[1] : 0;       /* nchain: one per symbol */
    for (i = 1; i < n; i++) {
        struct sym *s = &o->symtab[i];
        u32 v = o->base + s->value;

        if (s->shndx == SHN_UNDEF || (s->info & 15) == STT_TLS ||
            (s->info >> 4) == STB_LOCAL || v > a) {
            continue;
        }
        if (!best || v > o->base + best->value) {
            best = s;
        }
    }
    if (best) {
        info->dli_sname = o->strtab + best->name;
        info->dli_saddr = (void *)(o->base + best->value);
    }
    return 1;
}

static int iface_iterate_phdr(int (*cb)(struct dl_phdr_info_ *, unsigned long,
                                        void *),
                              void *data)
{
    int k, r = 0;

    for (k = 0; k < nobjs && !r; k++) {
        struct obj *o = &objs[k];
        struct dl_phdr_info_ info;
        u32 *dtv;

        mzero(&info, sizeof(info));
        info.dlpi_addr = o->base;
        info.dlpi_name = k ? o->path : "";     /* the program: "", as glibc */
        info.dlpi_phdr = o->phmem;
        info.dlpi_phnum = (unsigned short)o->phnum;
        info.dlpi_adds = (unsigned long long)nobjs;
        if (o->tls_memsz) {
            info.dlpi_tls_modid = (u32)k + 1;
            dtv = my_dtv();
            info.dlpi_tls_data = dtv ? (void *)dtv[k + 1] : 0;
        }
        r = cb(&info, sizeof(info), data);
    }
    return r;
}

static const struct dl_iface iface = {
    DL_IFACE_VERSION,
    &layout,
    iface_tls_block,
    iface_tls_release,
    iface_dlopen,
    iface_dlsym,
    iface_dlclose,
    iface_dladdr,
    iface_iterate_phdr,
};

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

    /* The program: already loaded, at the addresses it was linked for.
     * Named by argv[0], which is what dladdr can say about it. */
    objs[0].base = 0;
    {
        const char *a0 = ((char **)sp[2])[0];
        u32 n = a0 ? slen(a0) : 0;

        if (n >= sizeof(objs[0].path)) {
            n = sizeof(objs[0].path) - 1;
        }
        mcopy(objs[0].path, a0 ? a0 : "", n);
        objs[0].path[n] = '\0';
    }
    objs[0].phmem = ph;
    objs[0].phnum = (int)phnum;
    objs[0].lo = 0xffffffffu;
    for (i = 0; i < phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) {
            objs[0].dyn = (struct dyn *)ph[i].vaddr;
        }
        if (ph[i].type == PT_LOAD) {
            if (PAGE_DOWN(ph[i].vaddr) < objs[0].lo) {
                objs[0].lo = PAGE_DOWN(ph[i].vaddr);
            }
            if (PAGE_UP(ph[i].vaddr + ph[i].memsz) > objs[0].hi) {
                objs[0].hi = PAGE_UP(ph[i].vaddr + ph[i].memsz);
            }
        }
        if (ph[i].type == PT_TLS) {
            objs[0].tls_image = (const void *)ph[i].vaddr;
            objs[0].tls_filesz = ph[i].filesz;
            objs[0].tls_memsz = ph[i].memsz;
            objs[0].tls_align = ph[i].align ? ph[i].align : 1;
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
    tls_layout();               /* before relocating: TPREL needs it */
    for (k = nobjs - 1; k >= 1; k--) {
        relocate(&objs[k]);
    }
    relocate(&objs[0]);
    tls_first_thread();

    /* The C library's way back in here, for dlopen and late TLS. */
    {
        struct obj *def;
        struct sym *s = find_symbol("__sage040_dl", 1, 0, &def);

        if (s) {
            *(const struct dl_iface **)(def->base + s->value) = &iface;
        }
    }

    /* Initialisers, dependencies first. The program's own are its
     * crt0's business, as they are in a static program. */
    run_inits(1, nobjs);
    return entry;
}
