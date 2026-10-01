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
/*
 * What a setattr means. ATTR_MODE is the permission bits AND the
 * set-user-id, set-group-id and sticky bits -- the file TYPE bits are
 * never taken from the caller, because chmod may not turn a directory
 * into a file.
 */
#define ATTR_MODE   0x01
#define ATTR_UID    0x02
#define ATTR_GID    0x04

/* One line of /proc/mounts. */
struct mount_entry {
    char source[16];            /* "hda2"                               */
    char dir[256];              /* where, as an absolute path           */
    u32  flags;                 /* MS_RDONLY                            */
};

struct fs_type {
    const char *name;
    int (*mount)(struct blockdev *dev);
    int (*umount)(void);
    int (*open)(const char *path, int flags, struct file *f);
    int (*unlink)(const char *path);
    int (*rename)(const char *from, const char *to);
    int (*stat)(const char *path, struct stat *st);
    int (*readdir)(int index, struct dirent *d);
    int (*statfs)(const char *path, struct statfs *s);
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

    /*
     * A file's mode and ownership.
     *
     * One call with a MASK rather than a chmod and a chown, because
     * chown(2) sets a uid and a gid together and either may be "leave
     * it alone" -- and because the next thing to want changing this way
     * (a timestamp, a flag) then costs a mask bit rather than another
     * member. Only the bits named in `mask` are meant; the rest of the
     * arguments are not even read.
     *
     * The permission RULES are not here. A filesystem stores what it is
     * told; whether the caller was allowed to ask is settled in vfs.c
     * before this is reached, so a second filesystem cannot get the
     * policy subtly different.
     */
    int (*setattr)(const char *path, u32 mask, u32 mode, u32 uid, u32 gid);
    int (*fsetattr)(struct file *f, u32 mask, u32 mode, u32 uid, u32 gid);

    /* A second name for an existing file: one inode, two entries. A
     * filesystem that cannot (FAT) leaves this null and the VFS says
     * -EPERM, which is what link(2) reports for a filesystem that does
     * not support links. */
    int (*link)(const char *from, const char *to);

    /* A symbolic link: a file whose contents are a path. `readlink`
     * gives that path back without following it. A filesystem with no
     * such thing leaves both null and the VFS answers -EPERM for
     * making one and -EINVAL for reading one, which is what Linux
     * says. */
    int (*symlink)(const char *target, const char *linkpath);
    int (*readlink)(const char *path, char *out, u32 size);

    /* stat WITHOUT following a symlink in the last component: what
     * lstat(2) and `ls -l` mean. Null on a filesystem with no symlinks,
     * where it would be the same call -- the VFS falls back to stat. */
    int (*lstat)(const char *path, struct stat *st);

    /* Check the volume, and with FSCK_REPAIR put it right. */
    int (*check)(int flags, struct fsck_report *r);
    int (*label)(const char *path, struct fslabel *l);

    /*
     * Where on the disk byte `off` of an open file is: the sector, and
     * the device it is on. What swapon needs, to reach the swap file's
     * pages without going through the filesystem (swap.c).
     */
    int (*bmap)(struct file *f, u32 off, u32 *lba, struct blockdev **dev);

    /* An inode that is neither a file nor a directory: a FIFO, the one
     * kind the VFS asks for (it has nothing to put in a device node).
     * `mode` carries the type bits; the permission bits are final --
     * the umask is already off. Null: -EPERM. */
    int (*mknod)(const char *path, u32 mode);

    /* Called with the filesystem lock held, as the outermost holder is
     * about to let it go: a point between two calls, where a journal
     * may commit. Optional. */
    void (*boundary)(void);

    /*
     * MORE VOLUMES, on directories of this one: mount(2) and umount(2).
     * The root filesystem keeps its own mount table, because crossing
     * from one volume to the next happens in the middle of walking a
     * path, which is the filesystem's job. `list` gives entry i of it
     * (0 is the root volume) or -ENOENT past the end. Null on a
     * filesystem that cannot: the VFS says EINVAL.
     */
    int (*mount_on)(const char *dir, struct blockdev *b, u32 flags);
    int (*umount_on)(const char *dir, u32 flags);
    int (*mount_list)(int i, struct mount_entry *m);
    struct fs_type *next;
};

int vfs_register(struct fs_type *t);
struct fs_type *vfs_find(const char *name);
int vfs_root_has_modes(void);
void vfs_flusher(void);           /* the kjournald task: see vfs.c */

/* Mount `fsname` from `devname` at "/". Returns 0 or -errno. */
int vfs_mount(const char *fsname, const char *devname);
int vfs_umount(void);
void vfs_shutdown(void);
int vfs_mount_on(const char *source, const char *dir, const char *type,
                 u32 flags);
int vfs_umount_on(const char *dir, u32 flags);
int vfs_mount_list(int i, struct mount_entry *m);
int vfs_handles_in(u32 lo, u32 hi);   /* for fs/: is a cwd or root there */

/*
 * WHICH FILE, as one number: what the text cache, the lock table,
 * inotify, FIFOs and swap key on. The inode number alone stopped being
 * enough when a second volume could be mounted, because it numbers its
 * inodes from 1 as well; the device goes in the top bits. The root
 * volume's st_dev is 0, so its keys are its inode numbers, as before.
 */
static inline u32 vfs_file_key(const struct stat *st)
{
    return st->st_ino ^ ((u32)st->st_dev << 20);
}
int vfs_mounted(void);
const char *vfs_fs_name(void);
const char *vfs_dev_name(void);

/* ---------------------------------------------------------------- */
/* File descriptors                                                  */
/* ---------------------------------------------------------------- */

int  fd_open(const char *path, int flags);
/* With open(2)'s mode for a file it creates; VFS_NO_MODE for none. */
#define VFS_NO_MODE  0xffffffffUL
int  fd_open_mode(const char *path, int flags, u32 mode);
int  vfs_mkdir_mode(const char *path, u32 mode);
void vfs_trim_slashes(char *path);  /* "dir/" -> "dir", in place */
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
int  fd_install_file(struct file *f, int flags);

/*
 * For /proc (procfs.c). What an open file was opened as, recorded at
 * open: set by whatever opens one from a path, and named -- the path,
 * or "pipe:[N]" and the like for one that never had one -- when a
 * program reads /proc/<pid>/fd.
 */
void vfs_file_set_path(struct file *f, const char *path);
void vfs_file_set_name(struct file *f, const char *name);
int  vfs_file_name(struct file *f, char *out, u32 size);
int  vfs_file_stat(struct file *f, struct stat *st);
/* At an offset, without moving the position or honouring O_APPEND. */
s32  vfs_file_pread(struct file *f, u32 off, void *buf, u32 len);
s32  vfs_file_pwrite(struct file *f, u32 off, const void *buf, u32 len);
int  vfs_fsync(int fd);
s32  fd_pread(int fd, void *buf, u32 len, u32 off);
s32  fd_pwrite(int fd, const void *buf, u32 len, u32 off);

/* `path` from the caller's working directory, absolute, with "." and
 * ".." resolved and no symbolic link followed. 0, or -errno. */
int  vfs_abspath(const char *path, char *out, u32 size);

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
int  vfs_statfs(const char *path, struct statfs *s);  /* 0: the root */
int  vfs_check(int flags, struct fsck_report *r);
int  vfs_label(const char *path, struct fslabel *l);
int  vfs_flock(int fd, int op);
u32  flock_key(struct file *f);      /* what identifies a file to a lock */
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
void vfs_cred(u32 *uid, u32 *gid);      /* the caller's effective ids */

/*
 * May the caller do `want` (R_OK/W_OK/X_OK) to this path, or to the
 * directory it names an entry in? Both check SEARCH permission on every
 * directory above the target first. See the head of the section in
 * vfs.c: the rules are Unix's, including the two that surprise people.
 */
int  vfs_may(const char *path, int want);
/* chmod and chown in one, by mask. Who may is decided here; see vfs.c. */
int  vfs_link(const char *from, const char *to);
int  vfs_symlink(const char *target, const char *linkpath);
int  vfs_mknod(const char *path, u32 mode);
int  vfs_memfd(const char *name, u32 flags);
int  vfs_readlink(const char *path, char *out, u32 size);
int  vfs_lstat(const char *path, struct stat *st);
int  vfs_setattr(const char *path, u32 mask, u32 mode, u32 uid, u32 gid);
int  vfs_fsetattr(int fd, u32 mask, u32 mode, u32 uid, u32 gid);
int  vfs_may_parent(const char *path, int want);
int  vfs_chroot(const char *path);
void vfs_cwd_set(u32 ino, const char *path);
const char *vfs_cwd_path(void);

/* The root volume's journal (fs/ext2.c): kstat's numbers and test knob. */
void ext2_journal_stats(struct journalstats *js);
void ext2_journal_stop(u32 how, u32 n);

#endif /* VFS_H */
