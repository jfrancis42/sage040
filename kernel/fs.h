/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fs.h - the kernel's filesystem interface.
 *
 * The disk is a real MS-DOS disk -- an MBR partition table and a FAT16
 * volume -- so the same files can be read and written by Linux with
 * mtools, or by MS-DOS itself.  That constraint is the whole point: it
 * means the host can put a file on the disk and the kernel can read it,
 * with no format of our own invention in between.
 *
 * Names are 8.3 and case-insensitive, and only the root directory
 * exists.  Long names and subdirectories are the next two steps; the
 * calls below are the ones that do not change when they arrive.
 */
#ifndef FS_H
#define FS_H

#include "kernel.h"

#define FS_MAX_OPEN    8
#define FS_NAME_MAX    13      /* "12345678.123" and a NUL */

/* fs_open() flags */
#define O_READ         0x01
#define O_WRITE        0x02
#define O_RDWR         (O_READ | O_WRITE)
#define O_CREATE       0x04    /* make it if it does not exist */
#define O_TRUNC        0x08    /* discard any existing contents */
#define O_APPEND       0x10    /* every write goes to the end */

/* fs_seek() origins */
#define SEEK_SET       0
#define SEEK_CUR       1
#define SEEK_END       2

/* Directory entry attributes, as MS-DOS defines them. */
#define FS_ATTR_RDONLY 0x01
#define FS_ATTR_HIDDEN 0x02
#define FS_ATTR_SYSTEM 0x04
#define FS_ATTR_VOLUME 0x08
#define FS_ATTR_DIR    0x10
#define FS_ATTR_ARCHIVE 0x20

/* Errors.  Every call returns one of these, negative, or a result >= 0. */
#define FS_OK          0
#define FS_EIO        -1       /* the disk said no */
#define FS_ENOENT     -2       /* no such file */
#define FS_EEXIST     -3       /* already there */
#define FS_EINVAL     -4       /* bad name or argument */
#define FS_ENOSPC     -5       /* volume full */
#define FS_EMFILE     -6       /* too many open files */
#define FS_EBADF      -7       /* not an open file handle */
#define FS_EACCES     -8       /* read-only file, or wrong open mode */
#define FS_ENOTMNT    -9       /* no filesystem mounted */
#define FS_EDIRFULL  -10       /* root directory has no free entry */
#define FS_EBUSY     -11       /* the file is open */

struct fs_dirent {
    char name[FS_NAME_MAX];
    u32  size;
    u16  cluster;
    u8   attr;
    u16  date;                 /* MS-DOS packed date */
    u16  time;                 /* MS-DOS packed time */
};

int         fs_mount(void);
int         fs_mounted(void);
const char *fs_label(void);
u32         fs_cluster_bytes(void);
u32         fs_total_bytes(void);
u32         fs_free_bytes(void);
const char *fs_strerror(int err);

int  fs_open(const char *name, int flags);
int  fs_close(int fd);
s32  fs_read(int fd, void *buf, u32 len);
s32  fs_write(int fd, const void *buf, u32 len);
s32  fs_seek(int fd, s32 offset, int whence);
s32  fs_tell(int fd);
s32  fs_filesize(int fd);
int  fs_sync(int fd);

int  fs_unlink(const char *name);
int  fs_rename(const char *from, const char *to);
int  fs_stat(const char *name, struct fs_dirent *out);

/* Walk the root directory.  index counts real entries from 0; returns
 * FS_ENOENT once there are no more. */
int  fs_readdir(int index, struct fs_dirent *out);

#endif /* FS_H */
