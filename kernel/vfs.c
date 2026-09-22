/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * vfs.c - path resolution, the mount table and the descriptor table.
 *
 * This is the layer that makes "swap the filesystem" and "swap the
 * console" true statements rather than intentions. Nothing above it ever
 * calls into FAT16 or into the UART by name: a descriptor carries a
 * `struct file_ops` and every operation goes through that.
 *
 * Path resolution is deliberately small. There are two mount points and
 * they are fixed:
 *
 *   /dev/<name>   a registered character device
 *   anything else a file on the mounted volume
 *
 * A real system would have a directory tree and a mount list to walk.
 * This one has a flat FAT16 root directory, so a tree would be a data
 * structure with one interior node -- the shape is honest about what is
 * underneath it, and the calls above it do not change when that stops
 * being true.
 */
#include "textcache.h"
#include "vfs.h"
#include "errno.h"
#include "task.h"
#include "string.h"
#include "wait.h"
#include "signal.h"

#define DEV_PREFIX     "/dev/"
#define DEV_PREFIX_LEN 5

static struct fs_type *types;
static struct fs_type *mounted_fs;
static struct blockdev *mounted_dev;
/*
 * Open files, and the descriptors that point at them.
 *
 * TWO TABLES, NOT ONE, and the split is what multitasking needed. A
 * `struct file` is an open file -- its position, its flags, the device
 * behind it -- and lives here, shared. A DESCRIPTOR is an index into a
 * per-task array of pointers to those, and lives in `struct task`.
 *
 * That is what makes a program inherit its parent's descriptors: the
 * child's array points at the same open files, so the two share a
 * position, and writing to a redirected stdout appends where the last
 * write left off instead of starting over. Conflating the two -- which
 * is what this was before there was more than one task -- makes that
 * impossible to express.
 *
 * Reference counted, because two tasks can hold the same open file and
 * the last one to let go is the one that closes it.
 */
#define FILE_MAX  128           /* open files, the whole machine */

static struct file files[FILE_MAX];

/* ---------------------------------------------------------------- */
/* Filesystem types                                                  */
/* ---------------------------------------------------------------- */

int vfs_register(struct fs_type *t)
{
    if (!t || !t->name || !t->open) {
        return -EINVAL;
    }
    if (vfs_find(t->name)) {
        return -EEXIST;
    }
    t->next = types;
    types = t;
    return 0;
}

struct fs_type *vfs_find(const char *name)
{
    struct fs_type *t;

    for (t = types; t; t = t->next) {
        if (strcmp(t->name, name) == 0) {
            return t;
        }
    }
    return 0;
}

int vfs_mount(const char *fsname, const char *devname)
{
    struct fs_type *t = vfs_find(fsname);
    struct blockdev *b = dev_find_block(devname);
    int err;

    if (!t) {
        return -ENODEV;
    }
    if (!b) {
        return -ENXIO;
    }
    if (mounted_fs) {
        return -EBUSY;
    }
    if (!t->mount) {
        return -ENOSYS;
    }

    err = t->mount(b);
    if (err < 0) {
        return err;
    }
    mounted_fs = t;
    mounted_dev = b;
    return 0;
}

int vfs_umount(void)
{
    int i;

    if (!mounted_fs) {
        return -EINVAL;
    }
    /* Refuse while anything is still open, rather than leaving
     * descriptors pointing at a filesystem that is no longer there. */
    for (i = 0; i < FILE_MAX; i++) {
        if (files[i].used && files[i].ops != 0 && files[i].priv != 0) {
            return -EBUSY;
        }
    }
    if (mounted_fs->sync) {
        mounted_fs->sync();
    }
    if (mounted_fs->umount) {
        mounted_fs->umount();
    }
    textcache_forget_all();
    mounted_fs = 0;
    mounted_dev = 0;
    return 0;
}

/*
 * The machine is stopping: write everything out and unmount whatever is
 * still open. There is nobody left to use a descriptor afterwards, and
 * unmounting is what marks the volume clean for the next boot.
 */
void vfs_shutdown(void)
{
    if (!mounted_fs) {
        return;
    }
    if (mounted_fs->sync) {
        mounted_fs->sync();
    }
    if (mounted_fs->umount) {
        mounted_fs->umount();
    }
    mounted_fs = 0;
    mounted_dev = 0;
}

int vfs_mounted(void)
{
    return mounted_fs != 0;
}

const char *vfs_fs_name(void)
{
    return mounted_fs ? mounted_fs->name : "none";
}

const char *vfs_dev_name(void)
{
    return mounted_dev ? mounted_dev->name : "none";
}

/* ---------------------------------------------------------------- */
/* Paths                                                             */
/* ---------------------------------------------------------------- */

/*
 * Is this a device path, and if so which device?  Returns the chardev,
 * or NULL if the path is an ordinary file name.
 */
static struct chardev *resolve_dev(const char *path)
{
    if (strncmp(path, DEV_PREFIX, DEV_PREFIX_LEN) != 0) {
        return 0;
    }
    return dev_find_char(path + DEV_PREFIX_LEN);
}

/*
 * THE LEADING SLASH IS LOAD-BEARING AND MUST BE PASSED DOWN.
 *
 * There used to be a strip_root() here that removed it before handing
 * the path to the filesystem, on the reasoning that the mounted volume
 * IS the root so the slash says nothing. That was true while the root
 * was the only directory anybody could stand in.
 *
 * It stopped being true when there was a working directory, and it
 * failed in the worst available way: path_walk() resolves a name with
 * no leading slash RELATIVE TO THE CURRENT DIRECTORY, so `/etc/rc`
 * arrived as `etc/rc` and resolved to `/etc/etc/rc` from inside /etc --
 * while working perfectly from the root, which is where everything was
 * tested. An absolute path silently meant something different
 * depending on where the caller happened to be.
 *
 * path_walk() has handled a leading slash all along: it resets to the
 * root and skips it. So the fix is to pass the path through unchanged,
 * and there is nothing here to replace strip_root with.
 */

/* ---------------------------------------------------------------- */
/* Descriptors                                                       */
/* ---------------------------------------------------------------- */

struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= OPEN_MAX || !current || !current->fds[fd]) {
        return 0;
    }
    return current->fds[fd];
}

/* An open file, not a descriptor. */
static struct file *file_alloc(void)
{
    int i;

    for (i = 0; i < FILE_MAX; i++) {
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(files[i]));
            files[i].used = 1;
            files[i].refs = 1;
            return &files[i];
        }
    }
    return 0;
}

void file_get(struct file *f)
{
    if (f) {
        f->refs++;
    }
}

static void flock_release(struct file *f);

void file_put(struct file *f)
{
    if (!f || !f->used) {
        return;
    }
    if (--f->refs > 0) {
        return;             /* somebody else still has it */
    }
    flock_release(f);       /* a lock lives exactly as long as this */
    if (f->ops && f->ops->close) {
        f->ops->close(f);
    }
    f->used = 0;
}

/* ---------------------------------------------------------------- */
/* flock                                                             */
/* ---------------------------------------------------------------- */

/*
 * BSD's advisory locks, as Linux has them: held by an open file
 * DESCRIPTION -- so a dup shares one, and a second open() of the same
 * file is a different holder that can be refused -- on the FILE, which
 * is to say its inode number. Shared locks coexist; an exclusive one
 * coexists with nothing another description holds. Released by
 * LOCK_UN or by the last close of the description.
 *
 * A file with no inode number, a device, is keyed by what it is instead.
 * Advisory means exactly that: nothing stops a program that does not
 * ask from reading or writing.
 */
#define FLOCK_MAX 32

static struct {
    struct file *holder;
    u32 key;
    int exclusive;
} flocks[FLOCK_MAX];

static struct waitq flock_wait;

static u32 flock_key(struct file *f)
{
    struct stat st;

    memset(&st, 0, sizeof(st));
    if (f->ops && f->ops->fstat) {
        f->ops->fstat(f, &st);
    }
    return st.st_ino ? st.st_ino : (u32)f->priv ^ 0x80000000UL;
}

static void flock_release(struct file *f)
{
    int i, any = 0;

    for (i = 0; i < FLOCK_MAX; i++) {
        if (flocks[i].holder == f) {
            flocks[i].holder = 0;
            any = 1;
        }
    }
    if (any) {
        wake_all(&flock_wait);
    }
}

int vfs_flock(int fd, int op)
{
    struct file *f = fd_get(fd);
    int want_ex, i, free_slot;
    u32 key;

    if (!f) {
        return -EBADF;
    }
    if (op & ~(LOCK_SH | LOCK_EX | LOCK_NB | LOCK_UN)) {
        return -EINVAL;
    }
    if (op & LOCK_UN) {
        flock_release(f);
        return 0;
    }
    if (!(op & (LOCK_SH | LOCK_EX)) || ((op & LOCK_SH) && (op & LOCK_EX))) {
        return -EINVAL;
    }
    want_ex = (op & LOCK_EX) != 0;
    key = flock_key(f);

    for (;;) {
        int conflict = 0;

        free_slot = -1;
        for (i = 0; i < FLOCK_MAX; i++) {
            if (!flocks[i].holder) {
                if (free_slot < 0) {
                    free_slot = i;
                }
                continue;
            }
            if (flocks[i].holder == f) {
                free_slot = i;      /* ours: converted in place */
                continue;
            }
            if (flocks[i].key == key && (want_ex || flocks[i].exclusive)) {
                conflict = 1;
            }
        }
        if (!conflict) {
            break;
        }
        if (op & LOCK_NB) {
            return -EWOULDBLOCK;
        }
        sleep_on(&flock_wait);
        if (signal_pending(current)) {
            return -EINTR;
        }
    }
    if (free_slot < 0) {
        return -ENOLCK;
    }
    /* A holder has one lock per file: drop any other entry for it. */
    for (i = 0; i < FLOCK_MAX; i++) {
        if (flocks[i].holder == f && i != free_slot) {
            flocks[i].holder = 0;
        }
    }
    flocks[free_slot].holder = f;
    flocks[free_slot].key = key;
    flocks[free_slot].exclusive = want_ex;
    wake_all(&flock_wait);      /* a downgrade may let a waiter in */
    return 0;
}

/* The lowest free descriptor in the current task, as Unix requires --
 * it is what makes `exec 2>&1`-style redirection work by closing a
 * descriptor and reopening into the same number. */
static int fd_alloc(void)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        if (!current->fds[i]) {
            return i;
        }
    }
    return -EMFILE;
}

int fd_bind(int fd, const struct file_ops *ops, void *priv, int flags)
{
    struct file *f;

    if (fd < 0 || fd >= OPEN_MAX || !ops) {
        return -EINVAL;
    }
    f = file_alloc();
    if (!f) {
        return -ENFILE;
    }
    f->ops = ops;
    f->priv = priv;
    f->flags = flags & ~O_CLOEXEC;

    if (current->fds[fd]) {
        file_put(current->fds[fd]);
    }
    current->fds[fd] = f;
    current->fd_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    return fd;
}

int fd_install(const struct file_ops *ops, void *priv, int flags)
{
    struct file *f;
    int fd = fd_alloc();

    if (fd < 0) {
        return fd;
    }
    f = file_alloc();
    if (!f) {
        return -ENFILE;
    }
    f->ops = ops;
    f->priv = priv;
    f->flags = flags & ~O_CLOEXEC;
    current->fds[fd] = f;
    current->fd_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    return fd;
}

/* ---------------------------------------------------------------- */
/* Open directories                                                  */
/* ---------------------------------------------------------------- */

/*
 * A directory descriptor carries the directory's identity -- the u32 a
 * filesystem keeps a working directory as -- in priv, biased by one so
 * that the root (0) is not a null pointer, and how far through it the
 * reader has got in pos. Nothing else: the entries are read from the
 * filesystem as they are asked for, so a directory changed while it is
 * open is seen as it is now, the way Linux behaves.
 */
static u32 dir_ino_of(struct file *f)
{
    return (u32)f->priv - 1;
}

static s32 dir_read(struct file *f, void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EISDIR;
}

static s32 dir_write(struct file *f, const void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EBADF;
}

/* Only back to the start, which is what rewinddir() is. */
static s32 dir_lseek(struct file *f, s32 offset, int whence)
{
    if (whence != SEEK_SET || offset < 0) {
        return -EINVAL;
    }
    f->pos = (u32)offset;
    return offset;
}

static int dir_close(struct file *f)
{
    (void)f;
    return 0;
}

static int dir_fstat(struct file *f, struct stat *st)
{
    u32 ino = dir_ino_of(f);

    st->st_mode = S_IFDIR | 0755;
    st->st_ino = ino ? ino : 1;
    return 0;
}

static const struct file_ops dir_ops = {
    dir_read,
    dir_write,
    dir_lseek,
    0,
    dir_close,
    dir_fstat,
    0,
    0,                          /* truncate: nothing to truncate */
};

static int dir_open(const char *path, int flags)
{
    u32 ino;
    int err;

    if (!mounted_fs->dir_ino || !mounted_fs->readdir_in) {
        return -ENOSYS;
    }
    err = mounted_fs->dir_ino(path, &ino);
    if (err < 0) {
        return err;
    }
    return fd_install(&dir_ops, (void *)(ino + 1), flags & ~O_ACCMODE);
}

int vfs_is_dir_file(struct file *f)
{
    return f && f->ops == &dir_ops;
}

/*
 * Fill `buf` with as many linux_dirent64 records as fit, from where the
 * descriptor has got to. Returns the bytes used, 0 at the end, or
 * -EINVAL if not even one record fits -- Linux's answers.
 *
 * FAT's root has no "." or ".." on disk and every other directory does,
 * so the root is given them here: a program should not have to know
 * which directory it is listing to be shown the same two entries.
 */
s32 vfs_getdents64(int fd, u8 *buf, u32 len)
{
    struct file *f = fd_get(fd);
    u32 used = 0, ino;
    int synth;

    if (!f) {
        return -EBADF;
    }
    if (!vfs_is_dir_file(f)) {
        return -ENOTDIR;
    }
    ino = dir_ino_of(f);
    synth = (ino == 0) ? 2 : 0;

    for (;;) {
        struct dirent d;
        struct linux_dirent64 *r;
        u32 n, reclen;
        int idx = (int)f->pos;

        if (idx < synth) {
            memset(&d, 0, sizeof(d));
            strcpy(d.d_name, idx == 0 ? "." : "..");
            d.d_mode = S_IFDIR;
            d.d_ino = 1;
        } else {
            int err = mounted_fs->readdir_in(ino, idx - synth, &d);

            if (err == -ENOENT) {
                break;
            }
            if (err < 0) {
                return used ? (s32)used : err;
            }
        }

        n = (u32)strlen(d.d_name);
        reclen = (19 + n + 1 + 7) & ~7UL;   /* header is 19 bytes */
        if (used + reclen > len) {
            if (used == 0) {
                return -EINVAL;
            }
            break;
        }
        r = (struct linux_dirent64 *)(buf + used);
        memset(r, 0, reclen);
        r->d_ino = d.d_ino ? d.d_ino : 1;
        r->d_off = idx + 1;
        r->d_reclen = (u16)reclen;
        r->d_type = S_ISDIR(d.d_mode) ? DT_DIR : DT_REG;
        memcpy(r->d_name, d.d_name, n + 1);
        used += reclen;
        f->pos++;
    }
    return (s32)used;
}

int fd_open(const char *path, int flags)
{
    struct chardev *cd;
    int fd, err;

    if (!path || !*path) {
        return -EINVAL;
    }
    if (strlen(path) > PATH_MAX) {
        return -ENAMETOOLONG;
    }

    /* A device node is not a file on the volume and does not need one to
     * be mounted, which is what lets the console work before the disk
     * has been looked at. */
    cd = resolve_dev(path);
    if (cd) {
        return fd_install(cd->ops, cd->priv, flags);
    }
    if (strncmp(path, DEV_PREFIX, DEV_PREFIX_LEN) == 0) {
        return -ENXIO;          /* under /dev, but no such device */
    }

    if (!mounted_fs) {
        return -ENODEV;
    }

    /*
     * A directory can be opened, to read it with getdents64 -- which is
     * what readdir() in any C library is -- but not written, and it is
     * not a file the filesystem's open() knows how to hand out.
     */
    {
        struct stat st;
        int exists = mounted_fs->stat && mounted_fs->stat(path, &st) == 0;

        if (exists && S_ISDIR(st.st_mode)) {
            if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT)) {
                return -EISDIR;
            }
            return dir_open(path, flags);
        }
        if (flags & O_DIRECTORY) {
            return exists ? -ENOTDIR : -ENOENT;
        }
        if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
            return -EEXIST;
        }
    }

    fd = fd_alloc();
    if (fd < 0) {
        return fd;
    }
    {
        struct file *f = file_alloc();

        if (!f) {
            return -ENFILE;
        }
        f->flags = flags & ~O_CLOEXEC;
        err = mounted_fs->open(path, flags & ~O_CLOEXEC, f);
        if (err < 0) {
            f->used = 0;
            return err;
        }
        current->fds[fd] = f;
        current->fd_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    }
    if (flags & O_TRUNC) {
        textcache_forget_fd(fd);
    }
    return fd;
}

int fd_close(int fd)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    current->fds[fd] = 0;
    current->fd_flags[fd] = 0;
    file_put(f);
    return 0;
}

/* The lowest free descriptor at or above `min`, pointing at what `fd`
 * does. A duplicate never inherits FD_CLOEXEC: that is the flag's
 * definition, and what makes `dup` the way to keep something open
 * across a spawn. */
static int dup_from(int fd, int min)
{
    struct file *f = fd_get(fd);
    int n;

    if (!f) {
        return -EBADF;
    }
    if (min < 0 || min >= OPEN_MAX) {
        return -EINVAL;
    }
    for (n = min; n < OPEN_MAX && current->fds[n]; n++) {
    }
    if (n == OPEN_MAX) {
        return -EMFILE;
    }
    file_get(f);
    current->fds[n] = f;
    current->fd_flags[n] = 0;
    return n;
}

int fd_dup(int fd)
{
    return dup_from(fd, 0);
}

int fd_fcntl(int fd, int cmd, u32 arg)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    switch (cmd) {
    case F_DUPFD:
        return dup_from(fd, (int)arg);
    case F_GETFD:
        return current->fd_flags[fd];
    case F_SETFD:
        current->fd_flags[fd] = (u8)(arg & FD_CLOEXEC);
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL:
        /* The access mode was fixed at open; only these two may change,
         * which is POSIX's rule as well as Linux's. */
        f->flags = (f->flags & ~(O_NONBLOCK | O_APPEND)) |
                   ((int)arg & (O_NONBLOCK | O_APPEND));
        return 0;
    default:
        return -EINVAL;
    }
}

int fd_dup2(int oldfd, int newfd)
{
    struct file *f = fd_get(oldfd);

    if (!f) {
        return -EBADF;
    }
    if (newfd < 0 || newfd >= OPEN_MAX) {
        return -EBADF;
    }
    if (oldfd == newfd) {
        return newfd;
    }
    if (current->fds[newfd]) {
        file_put(current->fds[newfd]);
    }
    file_get(f);
    current->fds[newfd] = f;
    current->fd_flags[newfd] = 0;
    return newfd;
}

/*
 * Give a new task copies of these descriptors.
 *
 * The open files are SHARED, not copied -- both tasks point at the same
 * struct file and therefore the same position. That is what a Unix
 * child inherits, and it is why two processes writing to the same
 * redirected output do not overwrite each other from the start.
 */
void fd_inherit(struct task *child, struct task *parent)
{
    int i;

    /* spawn is fork and exec in one, so a close-on-exec descriptor is
     * one the child never gets. */
    for (i = 0; i < OPEN_MAX; i++) {
        if (parent->fd_flags[i] & FD_CLOEXEC) {
            child->fds[i] = 0;
            continue;
        }
        child->fds[i] = parent->fds[i];
        file_get(child->fds[i]);
    }
}

/* fork's inheritance: every descriptor, close-on-exec ones included,
 * with its flag -- nothing is being exec'd yet. */
void fd_fork(struct task *child, struct task *parent)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        child->fds[i] = parent->fds[i];
        child->fd_flags[i] = parent->fd_flags[i];
        file_get(child->fds[i]);
    }
}

/* execve's: the close-on-exec descriptors go. */
void fd_exec(struct task *t)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        if (t->fds[i] && (t->fd_flags[i] & FD_CLOEXEC)) {
            file_put(t->fds[i]);
            t->fds[i] = 0;
        }
        t->fd_flags[i] = 0;
    }
}

void fd_close_all(struct task *t)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        if (t->fds[i]) {
            file_put(t->fds[i]);
            t->fds[i] = 0;
        }
        t->fd_flags[i] = 0;
    }
}

s32 fd_read(int fd, void *buf, u32 len)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        return -EACCES;
    }
    if (!f->ops->read) {
        return -EINVAL;
    }
    return f->ops->read(f, buf, len);
}

s32 fd_write(int fd, const void *buf, u32 len)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    /*
     * Whether O_RDONLY forbids this is the driver's call, not the
     * descriptor layer's: a regular file opened for reading must refuse,
     * while the console is bound to descriptors 0, 1 and 2 at once and a
     * terminal is not read-only in the way a file is. fat16's write
     * checks the flags; the tty does not.
     */
    if (!f->ops->write) {
        return -EINVAL;
    }
    {
        s32 n = f->ops->write(f, buf, len);

        /* A file that changes is not what textcache.c holds of it any
         * more. The same after a truncate, an O_TRUNC open, an unlink
         * and a rename: every way a file's contents or its inode number
         * can change goes through here or one of those. */
        if (n > 0) {
            textcache_forget_fd(fd);
        }
        return n;
    }
}

s32 fd_lseek(int fd, s32 offset, int whence)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    if (!f->ops->lseek) {
        return -ESPIPE;         /* a terminal, for instance */
    }
    return f->ops->lseek(f, offset, whence);
}

int fd_ioctl(int fd, u32 request, u32 arg)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    if (!f->ops->ioctl) {
        return -ENOTTY;
    }
    return f->ops->ioctl(f, request, arg);
}

/* ---------------------------------------------------------------- */
/* Operations that name a path                                       */
/* ---------------------------------------------------------------- */

int vfs_mkdir(const char *path)
{
    if (!mounted_fs || !mounted_fs->mkdir) {
        return -ENOSYS;
    }
    return mounted_fs->mkdir(path);
}

int vfs_rmdir(const char *path)
{
    if (!mounted_fs || !mounted_fs->rmdir) {
        return -ENOSYS;
    }
    return mounted_fs->rmdir(path);
}

int vfs_chdir(const char *path)
{
    if (!mounted_fs || !mounted_fs->chdir) {
        return -ENOSYS;
    }
    return mounted_fs->chdir(path);
}

const char *vfs_getcwd(void)
{
    if (!mounted_fs || !mounted_fs->getcwd) {
        return "/";
    }
    return mounted_fs->getcwd();
}

int vfs_unlink(const char *path)
{
    if (resolve_dev(path)) {
        return -EPERM;          /* device nodes are not files */
    }
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->unlink) {
        return -ENOSYS;
    }
    /* Before, while the name still leads to the inode number: once the
     * entry is gone its slot can be a different file's. */
    textcache_forget_path(path);
    return mounted_fs->unlink(path);
}

int vfs_rename(const char *from, const char *to)
{
    if (resolve_dev(from) || resolve_dev(to)) {
        return -EPERM;
    }
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->rename) {
        return -ENOSYS;
    }
    /* Both: the moved file's inode number is where its entry was, and a
     * file it replaces is going. */
    textcache_forget_path(from);
    textcache_forget_path(to);
    return mounted_fs->rename(from, to);
}

int vfs_stat(const char *path, struct stat *st)
{
    struct chardev *cd = resolve_dev(path);

    if (cd) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR | S_IRUSR | S_IWUSR;
        return 0;
    }
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->stat) {
        return -ENOSYS;
    }
    return mounted_fs->stat(path, st);
}

int vfs_ftruncate(int fd, u32 len)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    if (!f->ops || !f->ops->truncate) {
        return -EINVAL;
    }
    textcache_forget_fd(fd);
    return f->ops->truncate(f, len);
}

int vfs_check(int flags, struct fsck_report *r)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->check) {
        return -ENOSYS;
    }
    if (flags & FSCK_REPAIR) {
        textcache_forget_all();     /* a repair can change any file */
    }
    return mounted_fs->check(flags, r);
}

int vfs_readdir(int index, struct dirent *d)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->readdir) {
        return -ENOSYS;
    }
    return mounted_fs->readdir(index, d);
}

/*
 * Describe an open descriptor.
 *
 * Asked of the file itself rather than of a path, because the caller
 * may not have one -- and because the file's own answer is the current
 * one. A file written and not yet flushed is longer than its directory
 * entry says.
 */
int vfs_fstat(int fd, struct stat *st)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    memset(st, 0, sizeof(*st));

    if (f->ops && f->ops->fstat) {
        return f->ops->fstat(f, st);
    }

    /*
     * No opinion from the driver. A descriptor that exists and cannot
     * describe itself is reported as a character device of no size,
     * which is what an unknown stream is.
     */
    st->st_mode = S_IFCHR;
    return 0;
}

/*
 * access(), answered from stat().
 *
 * THE ANSWERS ARE HONEST RATHER THAN INVENTED. This filesystem has no
 * permission bits, so:
 *
 *   F_OK  does it exist -- a real answer
 *   R_OK  yes, if it exists; everything readable is readable by all
 *   W_OK  yes, if it exists; there is no read-only bit consulted here
 *   X_OK  whether it is executable, which on this machine is decided
 *         by the first four bytes of the file and not by a mode -- so
 *         it is answered the same way exec() answers it
 *
 * Returning a plausible "yes" for X_OK on a text file would be the kind
 * of lie a shell turns into a confusing error much later.
 */
static int looks_executable(const char *path)
{
    /*
     * The same question exec() asks, asked the same way: a FAT16 volume
     * has no execute bit, so the file's first four bytes decide. Asking
     * it here rather than duplicating exec's rule means access(X_OK)
     * and exec() can never disagree.
     */
    u8 magic[4];
    int fd = fd_open(path, O_RDONLY);
    s32 n;

    if (fd < 0) {
        return 0;
    }
    n = fd_read(fd, magic, sizeof(magic));
    fd_close(fd);

    return n == (s32)sizeof(magic) &&
           magic[0] == 0x7f && magic[1] == 'E' &&
           magic[2] == 'L'  && magic[3] == 'F';
}

int vfs_access(const char *path, int mode)
{
    struct stat st;
    int err = vfs_stat(path, &st);

    if (err < 0) {
        return err;
    }
    if (mode & X_OK) {
        if (S_ISDIR(st.st_mode)) {
            return 0;           /* a directory is "executable": you can cd */
        }
        if (!looks_executable(path)) {
            return -EACCES;
        }
    }
    return 0;
}

int vfs_statfs(struct statfs *s)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->statfs) {
        return -ENOSYS;
    }
    return mounted_fs->statfs(s);
}

int vfs_sync(void)
{
    if (!mounted_fs || !mounted_fs->sync) {
        return 0;
    }
    return mounted_fs->sync();
}

/* --- the working directory ------------------------------------------ */

u32 vfs_cwd_ino(void)
{
    return current ? current->cwd_ino : 0;
}

void vfs_cwd_set(u32 ino, const char *path)
{
    if (!current) {
        return;
    }
    current->cwd_ino = ino;
    if (path) {
        strncpy(current->cwd_path, path, PATH_MAX - 1);
        current->cwd_path[PATH_MAX - 1] = '\0';
    }
}

const char *vfs_cwd_path(void)
{
    return current ? current->cwd_path : "/";
}
