/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * uapi.h - the types and constants that cross the system call boundary.
 *
 * Everything here is part of the contract between the kernel and the
 * programs that call it, which is why it is separate from vfs.h: a
 * program needs `struct stat` and O_CREAT, and has no business seeing
 * `struct fs_type` or the descriptor table.
 *
 * Linux keeps the same split for the same reason, under the same name.
 * When programs are separately compiled, this is the header they get and
 * the only one.
 */
#ifndef UAPI_H
#define UAPI_H

/*
 * The types, and nothing else. A program that includes this gets the
 * system call ABI without the kernel's internals or the machine's
 * register map coming with it.
 */
#include "types.h"

#define NAME_MAX      12        /* "12345678.123" without the NUL     */
#define PATH_MAX      64
#define OPEN_MAX      8         /* file descriptors per system        */

/*
 * open() flags. The access mode is the low two bits, the way POSIX has
 * it -- which means O_RDONLY is zero and testing for it with & does not
 * work. Use (flags & O_ACCMODE).
 */
#define O_RDONLY      0x0000
#define O_WRONLY      0x0001
#define O_RDWR        0x0002
#define O_ACCMODE     0x0003
#define O_CREAT       0x0040
#define O_TRUNC       0x0200
#define O_APPEND      0x0400

/*
 * ioctl requests. FIONREAD is Linux's, with Linux's number, and it is
 * the one a program needs to ask "has a key been pressed" without
 * blocking on a read that may never return.
 */
#define FIONREAD      0x541B

/*
 * Framebuffer ioctls, on /dev/fb0.
 *
 * Drawing through ioctl rather than through system calls of its own: a
 * framebuffer is a device, the device model already carries it, and
 * putting a dozen graphics calls in the system call table would tie the
 * kernel's ABI to one kind of hardware. Linux does its framebuffer
 * control this way for the same reason.
 *
 * Numbers are in Linux's framebuffer range but are not Linux's calls --
 * Linux has no "draw a line" ioctl, because it expects a program to map
 * the memory and draw for itself. Mapping needs an MMU, which is off.
 */
#define FBIO_GETINFO  0x4600    /* struct fb_info out                 */
#define FBIO_SETMODE  0x4601    /* struct fb_mode in                  */
#define FBIO_POINT    0x4602    /* struct fb_point in                 */
#define FBIO_LINE     0x4603    /* struct fb_line in                  */
#define FBIO_RECT     0x4604    /* struct fb_rect in                  */
#define FBIO_CLEAR    0x4605    /* colour, by value                   */
#define FBIO_FLIP     0x4606    /* show the drawn buffer              */
#define FBIO_SYNC     0x4607    /* wait for the blitter               */
#define FBIO_PALETTE  0x4608    /* struct fb_palette in               */

struct fb_info {
    u32 width;
    u32 height;
    u32 bpp;
    u32 pitch;
    char name[16];
};

struct fb_mode {
    u32 width;
    u32 height;
    u32 bpp;
};

struct fb_point {
    s32 x, y;
    u32 colour;
};

struct fb_line {
    s32 x0, y0, x1, y1;
    u32 colour;
};

struct fb_rect {
    s32 x, y;
    s32 w, h;
    u32 colour;
    u32 filled;
};

struct fb_palette {
    u32 index;
    u32 rgb;                    /* 0x00RRGGBB */
};

/* lseek() origins */
#define SEEK_SET      0
#define SEEK_CUR      1
#define SEEK_END      2

/* st_mode, as far as this kernel has a use for it */
#define S_IFMT        0170000
#define S_IFREG       0100000
#define S_IFDIR       0040000
#define S_IFCHR       0020000
#define S_IFBLK       0060000
#define S_IRUSR       0000400
#define S_IWUSR       0000200

#define S_ISREG(m)    (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)    (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)    (((m) & S_IFMT) == S_IFCHR)

struct stat {
    u32    st_mode;
    u32    st_size;
    time_t st_mtime;
    u32    st_blocks;
};

struct dirent {
    char   d_name[NAME_MAX + 1];
    u32    d_size;
    u32    d_mode;
    time_t d_mtime;
};

struct statfs {
    u32  f_bsize;               /* cluster size                       */
    u32  f_blocks;              /* total clusters                     */
    u32  f_bfree;               /* free clusters                      */
    char f_label[12];
    const char *f_type;         /* "fat16"                            */
};

/* ---------------------------------------------------------------- */
/* System call numbers                                               */
/*                                                                    */
/* Part of the ABI, so they live here rather than in the kernel's own */
/* header: a program needs them and needs nothing else from it.       */
/* ---------------------------------------------------------------- */

#define __NR_exit        1
#define __NR_read        3
#define __NR_write       4
#define __NR_open        5
#define __NR_close       6
#define __NR_unlink     10
#define __NR_time       13
#define __NR_lseek      19
#define __NR_stime      25
#define __NR_rename     38
#define __NR_ioctl      54
#define __NR_reboot     88
#define __NR_statfs     99
#define __NR_stat      106
#define __NR_fsync     118
#define __NR_uname     122
#define __NR_getdents  141
#define __NR_sync      166      /* Linux has 36; 166 keeps it clear of
                                 * this table's own use of 36..38      */

/*
 * Above 400 are calls Linux does not have, numbered well clear of it so
 * that nothing here can be mistaken for the real thing.
 *
 * spawn() is not execve(). execve replaces the calling process, and
 * there are no processes here to replace: this loads a program, runs it,
 * and returns its exit status. When there are processes it becomes
 * fork + execve + waitpid, the caller keeps the same shape, and this
 * number goes away.
 */
#define __NR_spawn     400

/*
 * times() here returns ticks since boot and takes no struct tms: there
 * are no processes to account time to. The number is Linux's; the
 * meaning is the subset of it that makes sense.
 */
#define __NR_times      43
#define __NR_nanosleep 162

/* Standard descriptors, bound to the console at startup. */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/*
 * nanosleep()'s argument. The kernel's tick is 10 ms, so anything finer
 * than that rounds up to one tick -- a sleep that returns early is a
 * bug waiting to happen and one tick late is nothing.
 */
struct timespec {
    u32 tv_sec;
    u32 tv_nsec;
};

/* What uname() fills in. */
struct utsname {
    char sysname[16];
    char release[16];
    char machine[16];
    char version[32];
};

/* reboot() commands, Linux's magic values cut down to what is useful. */
#define RB_HALT_SYSTEM  0xcdef0123
#define RB_AUTOBOOT     0x01234567

#endif /* UAPI_H */
