/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * vfs.h - the filesystem-independent layer.
 *
 * Two things live here. One is `struct fs_type`: what a filesystem has
 * to provide to be mountable, so that FAT16 can be replaced by something
 * else without a single caller changing. The other is path resolution,
 * which is what decides that "/dev/console" is a character device and
 * "notes.txt" is a file on the mounted volume.
 *
 * The mount table has exactly two entries and they are fixed: "/dev" is
 * the device registry and "/" is whatever filesystem was mounted. That
 * is enough to make the distinction the system call layer needs, and
 * short of a real directory tree there is nothing more to arrange.
 *
 * Names are Linux's -- open, close, read, write, lseek, stat, unlink,
 * rename, getdents -- because there is no reason to invent different
 * ones and every reason not to.
 */
#ifndef VFS_H
#define VFS_H

#include "kernel.h"
#include "uapi.h"
#include "dev.h"

/*
 * What a filesystem must implement.
 *
 * open() is handed a `struct file` to fill in: it sets ops and priv, and
 * everything after that goes through those. A filesystem that wants
 * different read/write behaviour per file -- a device node inside the
 * volume, say -- gets it for free that way.
 */
struct fs_type {
    const char *name;
    int (*mount)(struct blockdev *dev);
    int (*umount)(void);
    int (*open)(const char *path, int flags, struct file *f);
    int (*unlink)(const char *path);
    int (*rename)(const char *from, const char *to);
    int (*stat)(const char *path, struct stat *st);
    int (*readdir)(int index, struct dirent *d);
    int (*statfs)(struct statfs *s);
    int (*sync)(void);

    /*
     * Directories. A filesystem that has none leaves these null and the
     * VFS answers -ENOSYS, which is what a flat volume should say
     * rather than pretending a mkdir worked.
     */
    int (*mkdir)(const char *path);
    int (*rmdir)(const char *path);
    int (*chdir)(const char *path);
    const char *(*getcwd)(void);

    /*
     * Any directory, not just the working one: what an open directory
     * descriptor -- and so getdents64, and so readdir() in a C library
     * -- is built on. dir_ino names the directory `path` refers to by
     * the same u32 a working directory is kept as; readdir_in reads
     * entry `index` of it.
     */
    int (*readdir_in)(u32 ino, int index, struct dirent *d);
    int (*dir_ino)(const char *path, u32 *ino);
    /* And back again: the absolute path of directory `ino`, as it is
     * NOW -- what an *at call and fchdir resolve an open directory by. */
    int (*dir_path)(u32 ino, char *out, u32 size);

    /* A file's modification and access times, in seconds since 1970;
     * (u32)-1 leaves that one alone. By name, or by open file. */
    int (*utime)(const char *path, u32 mtime, u32 atime);
    int (*futime)(struct file *f, u32 mtime, u32 atime);

    /* Check the volume, and with FSCK_REPAIR put it right. */
    int (*check)(int flags, struct fsck_report *r);

    /*
     * Where on the disk byte `off` of an open file is: the sector, and
     * the device it is on. What swapon needs, to reach the swap file's
     * pages without going through the filesystem (swap.c).
     */
    int (*bmap)(struct file *f, u32 off, u32 *lba, struct blockdev **dev);

    struct fs_type *next;
};

int vfs_register(struct fs_type *t);
struct fs_type *vfs_find(const char *name);

/* Mount `fsname` from `devname` at "/". Returns 0 or -errno. */
int vfs_mount(const char *fsname, const char *devname);
int vfs_umount(void);
void vfs_shutdown(void);
int vfs_mounted(void);
const char *vfs_fs_name(void);
const char *vfs_dev_name(void);

/* ---------------------------------------------------------------- */
/* File descriptors                                                  */
/* ---------------------------------------------------------------- */

int  fd_open(const char *path, int flags);
int  fd_close(int fd);
s32  fd_read(int fd, void *buf, u32 len);
s32  fd_write(int fd, const void *buf, u32 len);
s32  fd_lseek(int fd, s32 offset, int whence);
int  fd_ioctl(int fd, u32 request, u32 arg);
struct file *fd_get(int fd);

/* Bind a descriptor to an already-open device. Used once at startup to
 * make 0, 1 and 2 the console before anything tries to print. */
int  fd_bind(int fd, const struct file_ops *ops, void *priv, int flags);

/*
 * Take the next free descriptor for something that is already open.
 *
 * What fd_open() does for a path, for things that do not have one: a
 * socket is the case that needed it. Returns the descriptor or -errno.
 */
int  fd_install(const struct file_ops *ops, void *priv, int flags);

/*
 * An OPEN FILE, which is not the same thing as a descriptor.
 *
 * Several descriptors -- in one task or in several -- may point at one
 * of these and share its position. Reference counted, so the last one to
 * let go is the one that actually closes it.
 */
void file_get(struct file *f);
void file_put(struct file *f);

int  fd_dup(int fd);
int  fd_fcntl(int fd, int cmd, u32 arg);
int  fd_dup2(int oldfd, int newfd);

/*
 * A DESCRIPTOR TABLE, which is not the same thing as a process.
 *
 * A process has one; the threads of one process share one, because a
 * descriptor opened by any thread is a descriptor every thread has
 * (POSIX, and what CLONE_FILES means). Reference counted like an open
 * file, and for the same reason: the last holder closes what is left.
 *
 * The tables come from a static pool of TASK_MAX, because a table is
 * only ever wanted by a task and there cannot be more tasks than that.
 */
struct fdtable {
    int refs;                   /* 0 when the entry is free            */
    struct file *fd[OPEN_MAX];
    u8    flags[OPEN_MAX];      /* FD_CLOEXEC: per descriptor, not per
                                 * open file -- see fcntl() in vfs.c    */
};

struct task;
struct fdtable *fdtable_alloc(void);   /* a new empty table, or null   */
void fdtable_get(struct fdtable *ft);  /* one more holder              */
void fdtable_put(struct fdtable *ft);  /* one fewer; closes at zero    */

void fd_inherit(struct task *child, struct task *parent);
void fd_fork(struct task *child, struct task *parent);
void fd_exec(struct task *t);
void fd_close_all(struct task *t);

/* Operations that name a path rather than a descriptor. */
int  vfs_unlink(const char *path);
int  vfs_mkdir(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_chdir(const char *path);
const char *vfs_getcwd(void);
int  vfs_rename(const char *from, const char *to);
int  vfs_stat(const char *path, struct stat *st);
int  vfs_fstat(int fd, struct stat *st);
int  vfs_bmap(int fd, u32 off, u32 *lba, struct blockdev **dev);
int  vfs_access(const char *path, int mode);
int  vfs_readdir(int index, struct dirent *d);
s32  vfs_getdents64(int fd, u8 *buf, u32 len);
int  vfs_is_dir_file(struct file *f);
int  vfs_dir_path(struct file *f, char *out, u32 size);
int  vfs_fchdir(int fd);
int  vfs_utime(const char *path, u32 mtime, u32 atime);
int  vfs_futime(int fd, u32 mtime, u32 atime);
int  vfs_statfs(struct statfs *s);
int  vfs_check(int flags, struct fsck_report *r);
int  vfs_flock(int fd, int op);
int  vfs_ftruncate(int fd, u32 len);
int  vfs_sync(void);


/*
 * The calling task's working directory.
 *
 * The filesystem owns the MEANING of `ino` -- for FAT16 it is a cluster
 * number, with 0 meaning the fixed root -- and the task layer owns the
 * storage, because a working directory belongs to a task the same way
 * its descriptors do. These two functions are the seam, which is why
 * fs/ does not include task.h.
 *
 * This used to be a single static in fs/fat16.c, so a chdir() anywhere
 * moved every task's idea of where it was, including the shell's.
 */
u32  vfs_cwd_ino(void);
u32  vfs_root_ino(void);                /* where "/" is for this task */
int  vfs_chroot(const char *path);
void vfs_cwd_set(u32 ino, const char *path);
const char *vfs_cwd_path(void);

#endif /* VFS_H */
