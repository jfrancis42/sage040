/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * coredump.c - a dying program's memory and registers, as a file.
 *
 * WHEN. A program ended by one of the signals whose default action is
 * "core" on Linux -- SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE,
 * SIGSEGV, SIGSYS, SIGXCPU, SIGXFSZ -- writes `core` in its working
 * directory, if its RLIMIT_CORE allows (Linux's default, 0, does not:
 * `ulimit -c unlimited` first), and if it is not running set-user-id or
 * set-group-id, whose memory may hold what its caller should not see.
 * The wait status then has WCOREDUMP (0x80) set.
 *
 * WHAT. Linux/m68k's ELF core, exactly, because the reader is gdb and
 * gdb already knows that format -- bfd's elf32-m68k recognises the
 * notes by their SIZE, so the sizes are not negotiable:
 *
 *   NT_PRSTATUS (154 bytes): the signal, the pids, and the registers in
 *     user_regs_struct order -- d1-d7, a0-a6, d0, usp, orig_d0, then
 *     stkadj and sr as two halves of one word, pc, format/vector.
 *     m68k aligns an int to two bytes, which is why it is 154 and not
 *     156: there is no padding after pr_cursig.
 *   NT_PRFPREG (108 bytes): fp0-fp7 as 96-bit extended, fpcr, fpsr,
 *     fpiar -- the order fmovem stores them.
 *   NT_PRPSINFO (124 bytes): the program's name; uid and gid are 16 bits
 *     in this one, as they were in Linux/m68k's first ABI.
 *
 * then one PT_LOAD for each run of pages /proc/<pid>/maps would list. A
 * WRITABLE run's contents are in the file; a read-only one's are not
 * (p_filesz 0), as Linux's default coredump_filter leaves them out too:
 * they are the program's and the libraries' text, which gdb reads from
 * the files themselves.
 *
 * The file is created as the program, through the program's own working
 * directory, so it lands where Linux's default core_pattern puts it and
 * only where the program could have written a file itself. An existing
 * `core` is replaced only if it is the program's own regular file.
 */

#include "coredump.h"
#include "task.h"
#include "vfs.h"
#include "vm.h"
#include "pmm.h"
#include "uaccess.h"
#include "errno.h"
#include "string.h"

extern void fpu_save(u32 *area);    /* taskasm.s */

#define CORE_REGIONS 48

struct region {
    u32 start, end;
    int prot;
};

struct collect {
    struct region *r;
    int n;
};

static int collect_region(void *arg, u32 start, u32 end, int prot, int shared)
{
    struct collect *c = arg;

    (void)shared;
    if (c->n < CORE_REGIONS) {
        c->r[c->n].start = start;
        c->r[c->n].end = end;
        c->r[c->n].prot = prot;
        c->n++;
    }
    return 0;
}

static int dumps_core(int sig)
{
    switch (sig) {
    case SIGQUIT: case SIGILL: case SIGTRAP: case SIGABRT: case SIGBUS:
    case SIGFPE: case SIGSEGV: case SIGSYS: case SIGXCPU: case SIGXFSZ:
        return 1;
    default:
        return 0;
    }
}

/* Big-endian stores into a byte buffer: the notes are packed, and an
 * int at an odd-looking offset is the point. */
static void be16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}

/* One note: header, "CORE" padded to 8, the descriptor padded to 4. */
static u32 note(u8 *p, u32 type, const u8 *desc, u32 size)
{
    be32(p, 5);
    be32(p + 4, size);
    be32(p + 8, type);
    memcpy(p + 12, "CORE\0\0\0", 8);
    memcpy(p + 20, desc, size);
    memset(p + 20 + size, 0, (4 - size % 4) % 4);
    return 20 + ((size + 3) & ~3u);
}

#define NT_PRSTATUS  1
#define NT_PRFPREG   2
#define NT_PRPSINFO  3

#define PRSTATUS_SIZE 154
#define PRFPREG_SIZE  108
#define PRPSINFO_SIZE 124

/* Writes are cut off at the limit, as Linux cuts a core off at it. */
struct out {
    int fd;
    u32 pos, limit;
    int err;
};

static void out_write(struct out *o, const void *buf, u32 len)
{
    s32 n;

    if (o->err) {
        return;
    }
    if (o->limit != RLIM_INFINITY) {
        if (o->pos >= o->limit) {
            o->err = -EFBIG;
            return;
        }
        if (len > o->limit - o->pos) {
            len = o->limit - o->pos;
        }
    }
    n = fd_write(o->fd, buf, len);
    if (n != (s32)len) {
        o->err = n < 0 ? n : -EIO;
        return;
    }
    o->pos += len;
}

int core_dump(struct task *t, int sig, const u32 *dregs,
              u16 sr, u32 pc, u16 fmtvec)
{
    struct collect c;
    struct out o;
    struct stat st;
    u32 page, *fpu, i, off, nphdr;
    u8 *buf, *p, *notes, desc[160];     /* the largest note, PRSTATUS */
    u32 usp, nlen;

    if (!dumps_core(sig) || !t || !t->as || t->core_cur == 0 ||
        t->uid != t->euid || t->gid != t->egid) {
        return 0;
    }

    /* One page, from the allocator: the notes and the region list are
     * built in it, then it carries each page of memory out. Not static
     * -- writing the file sleeps, and another program may be dying
     * meanwhile -- and not on the stack, which has 8 KB. */
    page = pmm_alloc();
    if (!page) {
        return 0;
    }
    buf = (u8 *)page;

    /* usp now, while nothing has run since the program trapped. */
    __asm__ volatile("move.l %%usp,%0" : "=a"(usp));

    c.r = (struct region *)(buf + 2048);
    c.n = 0;
    vm_regions(t->as, collect_region, &c);

    o.fd = fd_open_mode("core", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (o.fd < 0) {
        pmm_free(page);
        return 0;
    }
    if (vfs_fstat(o.fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != t->euid || vfs_ftruncate(o.fd, 0) < 0) {
        fd_close(o.fd);
        pmm_free(page);
        return 0;
    }
    o.pos = 0;
    o.limit = t->core_cur;
    o.err = 0;

    /* --- the notes, built first: their length places everything else */
    notes = buf + 1024;
    nlen = 0;

    memset(desc, 0, PRSTATUS_SIZE);
    be32(desc + 0, (u32)sig);                   /* pr_info.si_signo  */
    be16(desc + 12, (u32)sig);                  /* pr_cursig         */
    be32(desc + 22, (u32)t->pid);
    be32(desc + 26, t->parent ? (u32)t->parent->pid : 0);
    be32(desc + 30, (u32)t->pgid);
    be32(desc + 34, (u32)t->sid);
    p = desc + 70;                              /* pr_reg            */
    for (i = 1; i < 8; i++) {
        be32(p, dregs[i]); p += 4;              /* d1-d7             */
    }
    for (i = 8; i < 15; i++) {
        be32(p, dregs[i]); p += 4;              /* a0-a6             */
    }
    be32(p, dregs[0]); p += 4;                  /* d0                */
    be32(p, usp); p += 4;
    be32(p, dregs[0]); p += 4;                  /* orig_d0           */
    be16(p, 0); be16(p + 2, sr); p += 4;        /* stkadj, sr        */
    be32(p, pc); p += 4;
    be16(p, fmtvec); be16(p + 2, 0);            /* format, __fill    */
    be32(desc + 150, 1);                        /* pr_fpvalid        */
    nlen += note(notes + nlen, NT_PRSTATUS, desc, PRSTATUS_SIZE);

    /* The FPU as the program left it: switching is eager, so what is in
     * the FPU now is this task's. fpu_save's layout is taskasm.s's. */
    fpu = (u32 *)(buf + 3584);
    fpu_save(fpu);
    memcpy(desc, (u8 *)fpu + 100, 96);
    memcpy(desc + 96, (u8 *)fpu + 196, 12);
    nlen += note(notes + nlen, NT_PRFPREG, desc, PRFPREG_SIZE);

    memset(desc, 0, PRPSINFO_SIZE);
    desc[1] = 'R';                              /* pr_sname          */
    be16(desc + 8, t->uid);
    be16(desc + 10, t->gid);
    be32(desc + 12, (u32)t->pid);
    be32(desc + 16, t->parent ? (u32)t->parent->pid : 0);
    be32(desc + 20, (u32)t->pgid);
    be32(desc + 24, (u32)t->sid);
    strncpy((char *)desc + 28, t->name, 15);
    strncpy((char *)desc + 44, t->name, 79);
    nlen += note(notes + nlen, NT_PRPSINFO, desc, PRPSINFO_SIZE);

    /* --- the ELF header and program headers ------------------------- */
    nphdr = 1 + (u32)c.n;
    memset(buf, 0, 1024);
    memcpy(buf, "\177ELF", 4);
    buf[4] = 1;                                 /* ELFCLASS32        */
    buf[5] = 2;                                 /* ELFDATA2MSB       */
    buf[6] = 1;                                 /* EV_CURRENT        */
    be16(buf + 16, 4);                          /* ET_CORE           */
    be16(buf + 18, 4);                          /* EM_68K            */
    be32(buf + 20, 1);
    be32(buf + 28, 52);                         /* e_phoff           */
    be16(buf + 40, 52);                         /* e_ehsize          */
    be16(buf + 42, 32);                         /* e_phentsize       */
    be16(buf + 44, nphdr);
    out_write(&o, buf, 52);

    off = 52 + nphdr * 32;                      /* the notes go here */
    memset(buf, 0, 32);
    be32(buf, 4);                               /* PT_NOTE           */
    be32(buf + 4, off);
    be32(buf + 16, nlen);
    out_write(&o, buf, 32);

    off = (off + nlen + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (i = 0; i < (u32)c.n; i++) {
        struct region *r = &c.r[i];
        u32 size = r->end - r->start;
        int dumped = r->prot == VM_WRITE;

        memset(buf, 0, 32);
        be32(buf, 1);                           /* PT_LOAD           */
        be32(buf + 4, dumped ? off : 0);
        be32(buf + 8, r->start);
        be32(buf + 16, dumped ? size : 0);
        be32(buf + 20, size);
        be32(buf + 24, r->prot == VM_NONE ? 0 :
                       4 | (r->prot == VM_WRITE ? 2 : 1));  /* R, W|X */
        be32(buf + 28, PAGE_SIZE);
        out_write(&o, buf, 32);
        if (dumped) {
            off += size;
        }
    }
    out_write(&o, notes, nlen);

    /* --- the memory: each writable run, page by page, from the page
     * boundary after the notes. The page buffer is reused, so the
     * region list is copied out of it first. */
    {
        struct region regs[CORE_REGIONS];
        u32 n = (u32)c.n, pad;

        memcpy(regs, c.r, n * sizeof(regs[0]));
        pad = ((o.pos + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - o.pos;
        memset(buf, 0, PAGE_SIZE);
        out_write(&o, buf, pad);
        for (i = 0; i < n && !o.err; i++) {
            u32 va;

            if (regs[i].prot != VM_WRITE) {
                continue;
            }
            for (va = regs[i].start; va < regs[i].end && !o.err; va += PAGE_SIZE) {
                if (copy_from_user(buf, va, PAGE_SIZE) < 0) {
                    memset(buf, 0, PAGE_SIZE);
                }
                out_write(&o, buf, PAGE_SIZE);
            }
        }
    }

    fd_close(o.fd);
    pmm_free(page);
    /* Cut short by the limit is still a core, as on Linux. */
    return o.err == 0 || o.err == -EFBIG;
}
