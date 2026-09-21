/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * exec.c - the ELF loader.
 *
 * Reference: Tool Interface Standard, Executable and Linking Format,
 * version 1.2 (the 32-bit chapters).
 *
 * Programs are ELF32, big-endian, EM_68K, ET_EXEC -- the toolchain's own
 * output, so `make` produces something runnable with no objcopy step and
 * no format of our own to document. The boot ROM flattens the kernel
 * because it has to run before there is anything to parse a header with;
 * by the time a program is loaded there is a filesystem, a seek and room
 * to do it properly.
 *
 * Deliberately not supported: dynamic linking, relocation, shared
 * objects, interpreters. A program is statically linked at a fixed
 * address, which is the only thing that makes sense without an MMU
 * turned on.
 *
 * Every field is read a byte at a time. ELF32 for m68k is big-endian, so
 * a direct load would work -- but it would work by accident of the
 * header sitting at a convenient offset in a buffer, and the day someone
 * reads a phdr at an odd offset it stops working for reasons that take
 * an afternoon to find.
 */
#include "exec.h"
#include "vfs.h"
#include "console.h"
#include "errno.h"
#include "string.h"

/* --- ELF32 --------------------------------------------------------- */

#define EI_NIDENT     16
#define EHDR_SIZE     52
#define PHDR_SIZE     32

#define ELFCLASS32    1
#define ELFDATA2MSB   2         /* big-endian */
#define EV_CURRENT    1
#define ET_EXEC       2
#define EM_68K        4
#define PT_LOAD       1

/* Header offsets, from the specification. */
#define E_TYPE        16
#define E_MACHINE     18
#define E_VERSION     20
#define E_ENTRY       24
#define E_PHOFF       28
#define E_PHENTSIZE   42
#define E_PHNUM       44

#define P_TYPE        0
#define P_OFFSET      4
#define P_VADDR       8
#define P_FILESZ      16
#define P_MEMSZ       20

static u16 be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

/* --- the one running program --------------------------------------- */

extern int exec_call(u32 entry, u32 stack_top, int argc, char **argv);
extern void exec_longjmp(int status) __attribute__((noreturn));

static int running;

int exec_running(void)
{
    return running;
}

int exec_exit(int status)
{
    if (!running) {
        /*
         * The shell has nowhere to exit to. Saying so is better than
         * halting the machine, which is what a program calling exit()
         * with no parent would otherwise amount to.
         */
        return -ENOSYS;
    }
    running = 0;
    exec_longjmp(status);
    return 0;                   /* not reached */
}

/* --- loading -------------------------------------------------------- */

static int read_at(int fd, u32 offset, void *buf, u32 len)
{
    s32 n;

    if (fd_lseek(fd, (s32)offset, SEEK_SET) < 0) {
        return -EIO;
    }
    n = fd_read(fd, buf, len);
    if (n < 0) {
        return (int)n;
    }
    if ((u32)n != len) {
        /* A file too short to hold the header it claims is not a program
         * this kernel can run, which is what ENOEXEC means. */
        return -ENOEXEC;
    }
    return 0;
}

/*
 * Is this an m68k executable this kernel can run?
 *
 * Checked in the order that gives the most useful complaint: the magic
 * number first, because "that is not a program" is the common case and
 * deserves a plain answer.
 */
static int check_ident(const u8 *e)
{
    if (e[0] != 0x7f || e[1] != 'E' || e[2] != 'L' || e[3] != 'F') {
        return -ENOEXEC;
    }
    if (e[4] != ELFCLASS32 || e[5] != ELFDATA2MSB) {
        return -ENOEXEC;
    }
    if (be16(&e[E_TYPE]) != ET_EXEC) {
        return -ENOEXEC;
    }
    if (be16(&e[E_MACHINE]) != EM_68K) {
        return -ENOEXEC;
    }
    if (be32(&e[E_VERSION]) != EV_CURRENT) {
        return -ENOEXEC;
    }
    return 0;
}

/* Load every PT_LOAD segment and return the entry point in *entry. */
static int load_image(int fd, u32 *entry)
{
    u8 ehdr[EHDR_SIZE];
    u8 phdr[PHDR_SIZE];
    u32 phoff;
    int phnum, phentsize, i, err;
    int loaded = 0;

    err = read_at(fd, 0, ehdr, sizeof(ehdr));
    if (err < 0) {
        return err;
    }
    err = check_ident(ehdr);
    if (err < 0) {
        return err;
    }

    phoff = be32(&ehdr[E_PHOFF]);
    phnum = (int)be16(&ehdr[E_PHNUM]);
    phentsize = (int)be16(&ehdr[E_PHENTSIZE]);
    *entry = be32(&ehdr[E_ENTRY]);

    if (phoff == 0 || phnum <= 0 || phentsize < PHDR_SIZE) {
        return -ENOEXEC;
    }
    if (*entry < USER_BASE || *entry >= USER_LIMIT) {
        return -ENOEXEC;
    }

    for (i = 0; i < phnum; i++) {
        u32 type, off, vaddr, filesz, memsz;
        s32 n;

        err = read_at(fd, phoff + (u32)i * (u32)phentsize,
                      phdr, PHDR_SIZE);
        if (err < 0) {
            return err;
        }

        type = be32(&phdr[P_TYPE]);
        if (type != PT_LOAD) {
            continue;
        }

        off = be32(&phdr[P_OFFSET]);
        vaddr = be32(&phdr[P_VADDR]);
        filesz = be32(&phdr[P_FILESZ]);
        memsz = be32(&phdr[P_MEMSZ]);

        if (memsz < filesz) {
            return -ENOEXEC;
        }
        /*
         * The program says where it wants to live and the kernel decides
         * whether to believe it. Without an MMU this bounds check is the
         * only thing standing between a mislinked program and the
         * kernel's own memory, so it is done before a single byte is
         * read rather than after.
         */
        if (vaddr < USER_BASE || vaddr + memsz > USER_LIMIT ||
            vaddr + memsz < vaddr) {
            return -ENOEXEC;
        }

        if (filesz > 0) {
            if (fd_lseek(fd, (s32)off, SEEK_SET) < 0) {
                return -EIO;
            }
            n = fd_read(fd, (void *)vaddr, filesz);
            if (n < 0) {
                return (int)n;
            }
            if ((u32)n != filesz) {
                return -ENOEXEC;
            }
        }
        /* Anything the file does not supply is .bss and must be zero. */
        if (memsz > filesz) {
            memset((void *)(vaddr + filesz), 0, memsz - filesz);
        }
        loaded++;
    }

    return loaded > 0 ? 0 : -ENOEXEC;
}

int exec_spawn(const char *path, int argc, char **argv)
{
    u32 entry = 0;
    int fd, err, status;

    if (running) {
        /* One at a time: execasm.s has room for one saved context, and a
         * second spawn would overwrite the first's way home. */
        return -EBUSY;
    }
    if (argc < 0 || argc > EXEC_MAX_ARGS) {
        return -E2BIG;
    }

    fd = fd_open(path, O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    err = load_image(fd, &entry);
    fd_close(fd);
    if (err < 0) {
        return err;
    }

    running = 1;
    status = exec_call(entry, USER_STACK_TOP, argc, argv);
    running = 0;

    /*
     * Whatever the program left on the disk should be on the disk. It
     * may have been stopped partway through by exit(), with buffers
     * still held, and the shell is about to prompt as though nothing
     * happened.
     */
    vfs_sync();
    return status;
}
