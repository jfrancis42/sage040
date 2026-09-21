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

#include "kernel.h"

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

#endif /* UAPI_H */
