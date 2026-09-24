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
#include "swap.h"
#include "vfs.h"
#include "errno.h"
#include "task.h"
#include "string.h"
#include "wait.h"
#include "signal.h"
#include "pty.h"

#define DEV_PREFIX     "/dev/"
#define DEV_PREFIX_LEN 5

static struct fs_type *types;
static struct fs_type *mounted_fs;

/*
 * THE FILESYSTEM LOCK (task 22).
 *
 * The filesystem was never re-entered because nothing in it ever slept:
 * a task in fat16.c ran to the end of its call before any other could
 * start one. With the disk interrupt-driven, a task waiting for a sector
 * sleeps IN THE MIDDLE of a FAT operation -- half a directory entry
 * written, a cluster claimed and not yet linked -- and another task
 * entering then would see and change that. So every call from here into
 * the filesystem holds this.
 *
 * Recursive, because the filesystem's calls reach back through here
 * (the text cache reading a file, a truncate checking the swap file).
 * Taken only for the filesystem's own files and calls: a pipe or a
 * terminal can block for ever, and must not do it holding this.
 *
 * Order: this, then the disk's own lock (ata.c). The disk never takes
 * this, and swap I/O takes the disk without it, so there is no cycle.
 */
static struct waitq fs_wait;
static struct task *fs_owner;
static int fs_depth;

static void fs_lock(void)
{
    for (;;) {
        u16 sr = irq_save();

        if (!fs_owner || fs_owner == current) {
            fs_owner = current;
            fs_depth++;
            irq_restore(sr);
            return;
        }
        irq_restore(sr);
        sleep_on(&fs_wait);
    }
}

static void fs_unlock(void)
{
    u16 sr = irq_save();

    if (--fs_depth == 0) {
        fs_owner = 0;
        irq_restore(sr);
        wake_all(&fs_wait);
        return;
    }
    irq_restore(sr);
}
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
#define FILE_MAX  256           /* open files, the whole machine */

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
    fs_lock();
    if (mounted_fs->sync) {
        mounted_fs->sync();
    }
    if (mounted_fs->umount) {
        mounted_fs->umount();
    }
    fs_unlock();
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
    fs_lock();
    if (mounted_fs->sync) {
        mounted_fs->sync();
    }
    if (mounted_fs->umount) {
        mounted_fs->umount();
    }
    fs_unlock();
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
    if (fd < 0 || fd >= OPEN_MAX || !current || !current->files->fd[fd]) {
        return 0;
    }
    return current->files->fd[fd];
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
        if (f->fs) {
            fs_lock();
        }
        f->ops->close(f);
        if (f->fs) {
            fs_unlock();
        }
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
#define FLOCK_MAX 64

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
        if (f->fs) {
            fs_lock();
        }
        f->ops->fstat(f, &st);
        if (f->fs) {
            fs_unlock();
        }
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
        if (!current->files->fd[i]) {
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

    if (current->files->fd[fd]) {
        file_put(current->files->fd[fd]);
    }
    current->files->fd[fd] = f;
    current->files->flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
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
    current->files->fd[fd] = f;
    current->files->flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
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
    0,                          /* mmap: not memory to map */
};

static int dir_open(const char *path, int flags)
{
    u32 ino;
    int err;

    if (!mounted_fs->dir_ino || !mounted_fs->readdir_in) {
        return -ENOSYS;
    }
    fs_lock();
    err = mounted_fs->dir_ino(path, &ino);
    fs_unlock();
    if (err < 0) {
        return err;
    }
    return fd_install(&dir_ops, (void *)(ino + 1), flags & ~O_ACCMODE);
}

int vfs_is_dir_file(struct file *f)
{
    return f && f->ops == &dir_ops;
}

/* Where an open directory is now -- worked out, not remembered, so it
 * follows the directory through a rename. */
int vfs_dir_path(struct file *f, char *out, u32 size)
{
    int r;

    if (!vfs_is_dir_file(f)) {
        return -ENOTDIR;
    }
    if (!mounted_fs || !mounted_fs->dir_path) {
        return -ENOSYS;
    }
    fs_lock();
    r = mounted_fs->dir_path(dir_ino_of(f), out, size);
    fs_unlock();
    return r;
}

int vfs_utime(const char *path, u32 mtime, u32 atime)
{
    int r;

    if (resolve_dev(path)) {
        return 0;               /* a device has no time to keep */
    }
    if (!mounted_fs || !mounted_fs->utime) {
        return -ENOSYS;
    }
    fs_lock();
    r = mounted_fs->utime(path, mtime, atime);
    fs_unlock();
    return r;
}

int vfs_futime(int fd, u32 mtime, u32 atime)
{
    struct file *f = fd_get(fd);
    int r;

    if (!f) {
        return -EBADF;
    }
    if (vfs_is_dir_file(f)) {
        char path[PATH_MAX];

        r = vfs_dir_path(f, path, sizeof(path));
        return r < 0 ? r : vfs_utime(path, mtime, atime);
    }
    if (!f->fs) {
        return 0;               /* a device, a pipe: nothing to keep */
    }
    if (!mounted_fs || !mounted_fs->futime) {
        return -ENOSYS;
    }
    fs_lock();
    r = mounted_fs->futime(f, mtime, atime);
    fs_unlock();
    return r;
}

int vfs_fchdir(int fd)
{
    struct file *f = fd_get(fd);
    char path[PATH_MAX];
    int r;

    if (!f) {
        return -EBADF;
    }
    r = vfs_dir_path(f, path, sizeof(path));
    if (r < 0) {
        return r;
    }
    vfs_cwd_set(dir_ino_of(f), path);
    return 0;
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
            int err;

            fs_lock();
            err = mounted_fs->readdir_in(ino, idx - synth, &d);
            fs_unlock();

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

static int path_is_swapfile(const char *path);
static int is_swapfile(int fd);

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
        int fd = fd_install(cd->ops, cd->priv, flags);
        struct file *f;

        if (fd < 0) {
            return fd;
        }
        f = fd_get(fd);
        /*
         * /dev/ptmx is not a device to open: opening it ALLOCATES a
         * pseudo-terminal pair and gives back the master of it, so the
         * descriptor just installed is pointed somewhere else entirely.
         * A slave, /dev/pts/N, is an ordinary device open -- but the
         * pty has to be told, so that it knows when the program on the
         * terminal has gone.
         */
        if (f && strcmp(cd->name, "ptmx") == 0) {
            int err = pty_open_master(f);

            if (err < 0) {
                fd_close(fd);
                return err;
            }
        } else if (f && strncmp(cd->name, "pts/", 4) == 0) {
            pty_slave_opened(cd->priv);
        }
        return fd;
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
        int exists;

        fs_lock();
        exists = mounted_fs->stat && mounted_fs->stat(path, &st) == 0;
        fs_unlock();

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
        /* Truncating happens inside the filesystem's open: refused
         * before it, not after. */
        if (exists && (flags & O_TRUNC) && path_is_swapfile(path)) {
            return -ETXTBSY;
        }

        /*
         * MAY THE CALLER. Before the filesystem is asked to do
         * anything, so that a refusal cannot have created or truncated
         * the file on its way to saying no.
         *
         * A file that is not there and O_CREAT is a change to the
         * DIRECTORY, so that is where the permission has to be -- and
         * it needs write AND search, because adding a name means
         * finding the place to put it.
         */
        {
            int want = 0;
            int acc = flags & O_ACCMODE;

            if (acc == O_RDONLY || acc == O_RDWR) {
                want |= R_OK;
            }
            if (acc == O_WRONLY || acc == O_RDWR || (flags & O_TRUNC)) {
                want |= W_OK;
            }
            if (exists) {
                err = vfs_may(path, want);
            } else if (flags & O_CREAT) {
                err = vfs_may_parent(path, W_OK | X_OK);
            } else {
                err = 0;        /* no such file: let it say ENOENT */
            }
            if (err < 0) {
                return err;
            }
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
        fs_lock();
        err = mounted_fs->open(path, flags & ~O_CLOEXEC, f);
        fs_unlock();
        f->fs = 1;
        if (err < 0) {
            f->used = 0;
            return err;
        }
        current->files->fd[fd] = f;
        current->files->flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
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
    current->files->fd[fd] = 0;
    current->files->flags[fd] = 0;
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
    for (n = min; n < OPEN_MAX && current->files->fd[n]; n++) {
    }
    if (n == OPEN_MAX) {
        return -EMFILE;
    }
    file_get(f);
    current->files->fd[n] = f;
    current->files->flags[n] = 0;
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
        return current->files->flags[fd];
    case F_SETFD:
        current->files->flags[fd] = (u8)(arg & FD_CLOEXEC);
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
    if (current->files->fd[newfd]) {
        file_put(current->files->fd[newfd]);
    }
    file_get(f);
    current->files->fd[newfd] = f;
    current->files->flags[newfd] = 0;
    return newfd;
}

/* --- the descriptor table ------------------------------------------- */

/*
 * A static pool: a table belongs to a task, and there cannot be more
 * tasks than TASK_MAX. Allocating one cannot therefore fail for any
 * reason a new task would not have failed for anyway.
 */
static struct fdtable fdtables[TASK_MAX];

struct fdtable *fdtable_alloc(void)
{
    int i, j;

    for (i = 0; i < TASK_MAX; i++) {
        if (fdtables[i].refs == 0) {
            for (j = 0; j < OPEN_MAX; j++) {
                fdtables[i].fd[j] = 0;
                fdtables[i].flags[j] = 0;
            }
            fdtables[i].refs = 1;
            return &fdtables[i];
        }
    }
    return 0;
}

void fdtable_get(struct fdtable *ft)
{
    if (ft) {
        ft->refs++;
    }
}

/*
 * One holder fewer. At zero every descriptor still open is closed --
 * which is what makes the last thread of a process the one that closes
 * its files, rather than the first to exit.
 */
void fdtable_put(struct fdtable *ft)
{
    int i;

    if (!ft || --ft->refs > 0) {
        return;
    }
    for (i = 0; i < OPEN_MAX; i++) {
        if (ft->fd[i]) {
            file_put(ft->fd[i]);
            ft->fd[i] = 0;
        }
        ft->flags[i] = 0;
    }
    ft->refs = 0;
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
        if (parent->files->flags[i] & FD_CLOEXEC) {
            child->files->fd[i] = 0;
            continue;
        }
        child->files->fd[i] = parent->files->fd[i];
        file_get(child->files->fd[i]);
    }
}

/* fork's inheritance: every descriptor, close-on-exec ones included,
 * with its flag -- nothing is being exec'd yet. */
void fd_fork(struct task *child, struct task *parent)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        child->files->fd[i] = parent->files->fd[i];
        child->files->flags[i] = parent->files->flags[i];
        file_get(child->files->fd[i]);
    }
}

/* execve's: the close-on-exec descriptors go. */
void fd_exec(struct task *t)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        if (t->files->fd[i] && (t->files->flags[i] & FD_CLOEXEC)) {
            file_put(t->files->fd[i]);
            t->files->fd[i] = 0;
        }
        t->files->flags[i] = 0;
    }
}

void fd_close_all(struct task *t)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        if (t->files->fd[i]) {
            file_put(t->files->fd[i]);
            t->files->fd[i] = 0;
        }
        t->files->flags[i] = 0;
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
    if (!f->fs) {
        return f->ops->read(f, buf, len);
    }
    {
        s32 r;

        fs_lock();
        r = f->ops->read(f, buf, len);
        fs_unlock();
        return r;
    }
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
    if (is_swapfile(fd)) {
        return -ETXTBSY;
    }
    {
        s32 n;

        if (f->fs) {
            fs_lock();
        }
        n = f->ops->write(f, buf, len);
        if (f->fs) {
            fs_unlock();
        }

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
    if (!f->fs) {
        return f->ops->lseek(f, offset, whence);
    }
    {
        s32 r;

        fs_lock();
        r = f->ops->lseek(f, offset, whence);
        fs_unlock();
        return r;
    }
}

int fd_ioctl(int fd, u32 request, u32 arg)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    /*
     * The name of the device this descriptor is open on, answered here
     * from the registry rather than by the driver: every character
     * device has one, none of them has to implement it, and a driver
     * added later cannot forget to. This is what ttyname(3) is built
     * on -- see the note in uapi.h.
     */
    if (request == TIOCGDEVNAME) {
        const char *name = dev_char_name(f);
        u32 n;

        if (!name) {
            return -ENOTTY;    /* not a character device: no name */
        }
        n = strlen(name);
        if (n >= TTYNAME_MAX) {
            return -ENAMETOOLONG;
        }
        /* arg is a kernel buffer of TTYNAME_MAX: the syscall layer
         * fetched and will store it, from its ioctl table. Zero the
         * whole of it, so no tail of kernel memory goes out with a
         * short name. */
        memset((void *)arg, 0, TTYNAME_MAX);
        memcpy((void *)arg, name, n);
        return 0;
    }
    if (!f->ops->ioctl) {
        return -ENOTTY;
    }
    if (!f->fs) {
        return f->ops->ioctl(f, request, arg);
    }
    {
        int r;

        fs_lock();
        r = f->ops->ioctl(f, request, arg);
        fs_unlock();
        return r;
    }
}

/* ---------------------------------------------------------------- */
/* Operations that name a path                                       */
/* ---------------------------------------------------------------- */

int vfs_mkdir(const char *path)
{
    if (!mounted_fs || !mounted_fs->mkdir) {
        return -ENOSYS;
    }
    {
        int r = vfs_may_parent(path, W_OK | X_OK);

        if (r < 0) {
            return r;
        }
        fs_lock();
        r = mounted_fs->mkdir(path);
        fs_unlock();
        return r;
    }
}

int vfs_rmdir(const char *path)
{
    if (!mounted_fs || !mounted_fs->rmdir) {
        return -ENOSYS;
    }
    {
        int r = vfs_may_parent(path, W_OK | X_OK);

        if (r < 0) {
            return r;
        }
        fs_lock();
        r = mounted_fs->rmdir(path);
        fs_unlock();
        return r;
    }
}

int vfs_chdir(const char *path)
{
    if (!mounted_fs || !mounted_fs->chdir) {
        return -ENOSYS;
    }
    {
        int r = vfs_may(path, X_OK);

        if (r < 0) {
            return r;
        }
        fs_lock();
        r = mounted_fs->chdir(path);
        fs_unlock();
        return r;
    }
}

const char *vfs_getcwd(void)
{
    if (!mounted_fs || !mounted_fs->getcwd) {
        return "/";
    }
    {
        const char *r;

        fs_lock();
        r = mounted_fs->getcwd();
        fs_unlock();
        return r;
    }
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
    if (path_is_swapfile(path)) {
        return -ETXTBSY;
    }
    /*
     * Removing a name changes the DIRECTORY, so that is what has to be
     * writable -- not the file. A read-only file in a directory you own
     * is yours to delete, which is Unix and surprises somebody every
     * time. Checked before the textcache is told anything, so a refused
     * unlink leaves no trace.
     */
    {
        int r = vfs_may_parent(path, W_OK | X_OK);

        if (r < 0) {
            return r;
        }
    }
    /* Before, while the name still leads to the inode number: once the
     * entry is gone its slot can be a different file's. */
    textcache_forget_path(path);
    {
        int r;

        fs_lock();
        r = mounted_fs->unlink(path);
        fs_unlock();
        return r;
    }
}

/*
 * link(2). The permission is on the DIRECTORY the new name goes in,
 * not on the file: making a second name for a file does not change the
 * file, and you need no right over it beyond being able to reach it.
 */
int vfs_link(const char *from, const char *to)
{
    int err;

    if (resolve_dev(from) || resolve_dev(to)) {
        return -EPERM;          /* device nodes are not files */
    }
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->link) {
        return -EPERM;          /* what link(2) says for a volume that
                                 * has no such thing */
    }
    err = vfs_may(from, 0);     /* reachable: search permission above it */
    if (err < 0) {
        return err;
    }
    err = vfs_may_parent(to, W_OK | X_OK);
    if (err < 0) {
        return err;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->link(from, to);
        fs_unlock();
        return r;
    }
}

/*
 * symlink(2). The permission is on the directory the link goes in.
 * Nothing is checked about the target -- it need not exist, and the
 * caller need have no right over it: a symlink grants nothing, it only
 * names something, and the check happens when somebody follows it.
 */
/*
 * lstat: the LINK, not what it points at.
 *
 * Falls back to stat on a filesystem that has no symlinks, where the
 * two questions have the same answer -- rather than making every such
 * filesystem write the same function twice.
 */
int vfs_lstat(const char *path, struct stat *st)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->lstat) {
        return vfs_stat(path, st);
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->lstat(path, st);
        fs_unlock();
        return r;
    }
}

int vfs_symlink(const char *target, const char *linkpath)
{
    int err;

    if (resolve_dev(linkpath)) {
        return -EEXIST;
    }
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->symlink) {
        return -EPERM;
    }
    err = vfs_may_parent(linkpath, W_OK | X_OK);
    if (err < 0) {
        return err;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->symlink(target, linkpath);
        fs_unlock();
        return r;
    }
}

/*
 * readlink(2). Needs only to REACH the link -- no read permission on
 * the link itself, whose mode is 0777 and means nothing, and none on
 * the target, which may not even exist.
 */
int vfs_readlink(const char *path, char *out, u32 size)
{
    int err;

    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->readlink) {
        return -EINVAL;         /* what readlink says for a non-link */
    }
    err = vfs_may_parent(path, X_OK);
    if (err < 0) {
        return err;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->readlink(path, out, size);
        fs_unlock();
        return r;
    }
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
    if (path_is_swapfile(from) || path_is_swapfile(to)) {
        return -ETXTBSY;
    }
    /* BOTH directories: a rename removes a name from one and adds one
     * to the other, and either may be refused. */
    {
        int r = vfs_may_parent(from, W_OK | X_OK);

        if (r < 0) {
            return r;
        }
        r = vfs_may_parent(to, W_OK | X_OK);
        if (r < 0) {
            return r;
        }
    }
    /* Both: the moved file's inode number is where its entry was, and a
     * file it replaces is going. */
    textcache_forget_path(from);
    textcache_forget_path(to);
    {
        int r;

        fs_lock();
        r = mounted_fs->rename(from, to);
        fs_unlock();
        return r;
    }
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
    {
        int r;

        fs_lock();
        r = mounted_fs->stat(path, st);
        fs_unlock();
        return r;
    }
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
    if (is_swapfile(fd)) {
        return -ETXTBSY;
    }
    textcache_forget_fd(fd);
    if (!f->fs) {
        return f->ops->truncate(f, len);
    }
    {
        int r;

        fs_lock();
        r = f->ops->truncate(f, len);
        fs_unlock();
        return r;
    }
}

/*
 * Is this descriptor the swap file? The kernel writes the swap file's
 * sectors directly (swap.c); anything written to it through the
 * filesystem would be overwritten, or overwrite a program's memory. So
 * while it is in use, Linux's ETXTBSY for any change to it.
 */
static int is_swapfile(int fd)
{
    struct stat st;

    return swap_is_on() && vfs_fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
           swap_holds(st.st_ino);
}

static int path_is_swapfile(const char *path)
{
    struct stat st;

    return swap_is_on() && vfs_stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           swap_holds(st.st_ino);
}

int vfs_bmap(int fd, u32 off, u32 *lba, struct blockdev **dev)
{
    struct file *f = fd_get(fd);
    struct stat st;

    if (!f) {
        return -EBADF;
    }
    if (!mounted_fs || !mounted_fs->bmap || !f->ops || !f->ops->fstat ||
        vfs_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        return -EINVAL;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->bmap(f, off, lba, dev);
        fs_unlock();
        return r;
    }
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
    {
        int rv;

        fs_lock();
        rv = mounted_fs->check(flags, r);
        fs_unlock();
        return rv;
    }
}

int vfs_readdir(int index, struct dirent *d)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->readdir) {
        return -ENOSYS;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->readdir(index, d);
        fs_unlock();
        return r;
    }
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
        if (!f->fs) {
            return f->ops->fstat(f, st);
        }
        {
            int r;

            fs_lock();
            r = f->ops->fstat(f, st);
            fs_unlock();
            return r;
        }
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

/* --- who may do what ------------------------------------------------- */
/*
 * THE ONE PLACE THAT DECIDES.
 *
 * Every permission question in this kernel comes here, so that a second
 * filesystem cannot get the policy subtly different and so that there
 * is exactly one piece of code to read when asking "why was I allowed
 * to do that". The rules are Unix's, and they are worth stating because
 * two of them surprise people:
 *
 *   - The three sets are tried in order OWNER, GROUP, OTHER and the
 *     FIRST match decides. They are not OR'd together. A file with
 *     mode 0077 is unreadable BY ITS OWNER and readable by everybody
 *     else, which looks like a bug and is the specification.
 *
 *   - Root (euid 0) may do anything, with one exception: it may only
 *     execute a file if SOMEBODY could, that is if any of the three x
 *     bits is set. Otherwise every data file on the disk would be a
 *     program to root.
 *
 * `want` is the R_OK/W_OK/X_OK bits, the same ones access(2) takes.
 */
static int perm_ok(const struct stat *st, int want)
{
    u32 mode = st->st_mode;
    u32 bits;
    int i;

    if (!current) {
        return 0;               /* early boot: nobody to check against */
    }

    if (current->euid == 0) {
        if (want & X_OK) {
            if (S_ISDIR(mode)) {
                return 0;
            }
            return (mode & 0111) ? 0 : -EACCES;
        }
        return 0;
    }

    if (current->euid == st->st_uid) {
        bits = (mode >> 6) & 7;
    } else {
        int member = (current->egid == st->st_gid);

        for (i = 0; !member && i < current->ngroups; i++) {
            if (current->groups[i] == st->st_gid) {
                member = 1;
            }
        }
        bits = member ? ((mode >> 3) & 7) : (mode & 7);
    }

    if ((want & R_OK) && !(bits & 4)) {
        return -EACCES;
    }
    if ((want & W_OK) && !(bits & 2)) {
        return -EACCES;
    }
    if ((want & X_OK) && !(bits & 1)) {
        return -EACCES;
    }
    return 0;
}

/*
 * May the caller reach `path` at all, and then do `want` to it?
 *
 * Reaching it means SEARCH permission on every directory above it --
 * the x bit on a directory, which is what makes a mode 0711 home
 * directory work: anyone may walk through it to a named file and
 * nobody may list it. Checking only the final object would leave a
 * private directory no protection at all beyond its own entry.
 *
 * Each prefix is stat'd in turn, so a path d deep costs d stats. They
 * come out of the block cache and this is not the expensive part of a
 * system call, but it is why the walk stops at the first refusal.
 */
static int walk_ok(const char *path)
{
    char dir[PATH_MAX];
    struct stat st;
    u32 i, n = 0;
    int err;

    if (!current || current->euid == 0) {
        return 0;               /* root searches anything that exists */
    }
    for (i = 0; path[i]; i++) {
        if (path[i] != '/' || i == 0) {
            continue;
        }
        if (i >= sizeof(dir)) {
            return -ENAMETOOLONG;
        }
        memcpy(dir, path, i);
        dir[i] = '\0';
        n++;
        err = vfs_stat(dir, &st);
        if (err < 0) {
            return err;
        }
        if (!S_ISDIR(st.st_mode)) {
            return -ENOTDIR;
        }
        err = perm_ok(&st, X_OK);
        if (err < 0) {
            return err;
        }
    }
    (void)n;
    return 0;
}

/*
 * The check every path-taking system call makes. Refuses before the
 * filesystem is asked to do anything, so a denied call cannot have a
 * side effect.
 */
int vfs_may(const char *path, int want)
{
    struct stat st;
    int err = walk_ok(path);

    if (err < 0) {
        return err;
    }
    err = vfs_stat(path, &st);
    if (err < 0) {
        return err;
    }
    return perm_ok(&st, want);
}

/*
 * The same question about the DIRECTORY a path names an entry in, which
 * is what creating, deleting and renaming actually need: those change
 * the directory, not the file. Unlink does not ask whether you may
 * write the file -- a read-only file in a directory you own is yours to
 * remove, which is Unix and surprises people every time.
 */
int vfs_may_parent(const char *path, int want)
{
    char dir[PATH_MAX];
    struct stat st;
    u32 i, cut = 0;
    int err;

    for (i = 0; path[i]; i++) {
        if (path[i] == '/') {
            cut = i;
        }
    }
    if (cut == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (cut >= sizeof(dir)) {
            return -ENAMETOOLONG;
        }
        memcpy(dir, path, cut);
        dir[cut] = '\0';
    }
    err = walk_ok(dir);
    if (err < 0) {
        return err;
    }
    err = vfs_stat(dir, &st);
    if (err < 0) {
        return err;
    }
    return perm_ok(&st, want);
}

/*
 * chmod and chown, and the rules about who may.
 *
 *   - Only the OWNER or root may change a mode. Not somebody with
 *     write permission: being allowed to change a file is not being
 *     allowed to change who else may.
 *
 *   - Only ROOT may give a file away. This is the "restricted chown"
 *     every modern Unix does; the alternative lets a user dodge a disk
 *     quota, and worse, plant a set-user-id file owned by somebody
 *     else.
 *
 *   - An owner may change the GROUP, but only to a group they are in.
 *
 *   - Changing the owner or group CLEARS set-user-id and set-group-id.
 *     Otherwise `chown root file` on a file somebody had already made
 *     set-user-id would hand them the machine. Root is not exempt:
 *     making root exempt is how this is usually got wrong.
 */
static int may_setattr(const struct stat *stp, u32 *maskp, u32 *modep,
                       u32 uid, u32 gid)
{
    const struct stat st = *stp;
    u32 mask = *maskp, mode = *modep;

    if (current && current->euid != 0) {
        if (current->euid != st.st_uid) {
            return -EPERM;
        }
        if ((mask & ATTR_UID) && uid != (u32)-1 && uid != st.st_uid) {
            return -EPERM;      /* only root gives a file away */
        }
        if ((mask & ATTR_GID) && gid != (u32)-1 && gid != st.st_gid) {
            int i, member = (current->egid == gid);

            for (i = 0; !member && i < current->ngroups; i++) {
                if (current->groups[i] == gid) {
                    member = 1;
                }
            }
            if (!member) {
                return -EPERM;
            }
        }
    }

    /* (u32)-1 means "leave this one": chown(path, -1, gid) is how a
     * group is changed by itself, and the caller's own value must not
     * be written back over the file's. */
    if ((mask & ATTR_UID) && uid == (u32)-1) {
        mask &= ~ATTR_UID;
    }
    if ((mask & ATTR_GID) && gid == (u32)-1) {
        mask &= ~ATTR_GID;
    }

    /* An ownership change drops set-user-id and set-group-id, for
     * everybody including root. */
    if ((mask & (ATTR_UID | ATTR_GID)) && !(mask & ATTR_MODE) &&
        (st.st_mode & (S_ISUID | S_ISGID))) {
        mask |= ATTR_MODE;
        mode = st.st_mode & ~(u32)(S_ISUID | S_ISGID);
    }
    *maskp = mask;
    *modep = mode;
    return 0;
}

int vfs_setattr(const char *path, u32 mask, u32 mode, u32 uid, u32 gid)
{
    struct stat st;
    int err;

    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->setattr) {
        return -ENOSYS;         /* a volume with nowhere to put it */
    }
    err = walk_ok(path);
    if (err < 0) {
        return err;
    }
    err = vfs_stat(path, &st);
    if (err < 0) {
        return err;
    }
    err = may_setattr(&st, &mask, &mode, uid, gid);
    if (err < 0) {
        return err;
    }
    if (mask == 0) {
        return 0;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->setattr(path, mask, mode, uid, gid);
        fs_unlock();
        return r;
    }
}

/*
 * The same, by open descriptor.
 *
 * Shares the rules with vfs_setattr through may_setattr() rather than
 * repeating them: two copies of "who may chown" is one copy that will
 * be wrong later. What it cannot share is the path walk -- a
 * descriptor is already open, so search permission was settled when it
 * was opened, which is exactly what an fd-based call means.
 */
int vfs_fsetattr(int fd, u32 mask, u32 mode, u32 uid, u32 gid)
{
    struct stat st;
    int err;

    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->fsetattr) {
        return -ENOSYS;
    }
    err = vfs_fstat(fd, &st);
    if (err < 0) {
        return err;
    }
    err = may_setattr(&st, &mask, &mode, uid, gid);
    if (err < 0) {
        return err;
    }
    if (mask == 0) {
        return 0;
    }
    {
        struct file *f = fd_get(fd);
        int r;

        if (!f) {
            return -EBADF;
        }
        fs_lock();
        r = mounted_fs->fsetattr(f, mask, mode, uid, gid);
        fs_unlock();
        return r;
    }
}

int vfs_access(const char *path, int mode)
{
    struct stat st;
    int err = vfs_stat(path, &st);

    if (err < 0) {
        return err;
    }
    err = walk_ok(path);
    if (err < 0) {
        return err;
    }
    /*
     * A file with no x bit is not a program however it starts, and a
     * file with one still has to look like one: exec.c decides what is
     * executable from the first four bytes, because that is the only
     * thing that can work where a name means nothing. Both have to
     * agree, or `access(X_OK)` would promise an exec that then fails.
     */
    if (mode & X_OK) {
        if (S_ISDIR(st.st_mode)) {
            return perm_ok(&st, X_OK);
        }
        err = perm_ok(&st, X_OK);
        if (err < 0) {
            return err;
        }
        if (!looks_executable(path)) {
            return -EACCES;
        }
        return 0;
    }
    return perm_ok(&st, mode & (R_OK | W_OK));
}

int vfs_statfs(struct statfs *s)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->statfs) {
        return -ENOSYS;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->statfs(s);
        fs_unlock();
        return r;
    }
}

/* The mounted volume's name. See FSCTL_LABEL in uapi.h. */
int vfs_label(struct fslabel *l)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->label) {
        return -ENOSYS;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->label(l);
        fs_unlock();
        return r;
    }
}

int vfs_sync(void)
{
    if (!mounted_fs || !mounted_fs->sync) {
        return 0;
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->sync();
        fs_unlock();
        return r;
    }
}

/* --- the working directory ------------------------------------------ */

u32 vfs_cwd_ino(void)
{
    return current ? current->cwd_ino : 0;
}

u32 vfs_root_ino(void)
{
    return current ? current->root_ino : 0;
}

/* Who is asking, for a filesystem that records an owner on the file it
 * is about to create. Here for the same reason vfs_cwd_ino() is: fs/
 * does not include task.h. */
void vfs_cred(u32 *uid, u32 *gid)
{
    *uid = current ? current->euid : 0;
    *gid = current ? current->egid : 0;
}

/* chroot(): absolute paths start at `path` from now on, for this task
 * and what it starts. The working directory does not move, as on Linux. */
int vfs_chroot(const char *path)
{
    u32 ino;
    int err;

    if (!current) {
        return -EPERM;
    }
    if (!mounted_fs || !mounted_fs->dir_ino) {
        return -ENOSYS;
    }
    fs_lock();
    err = mounted_fs->dir_ino(path, &ino);
    fs_unlock();
    if (err < 0) {
        return err;
    }
    {   /* chroot moves the process, threads and all -- see vfs_cwd_set. */
        struct task *t;
        int i;

        for (i = 0; (t = task_nth(i)) != 0; i++) {
            if (t->tgid == current->tgid) {
                t->root_ino = ino;
            }
        }
    }
    return 0;
}

/*
 * Move the working directory -- of every thread of this process.
 *
 * A working directory belongs to a PROCESS, so a thread that chdirs
 * moves all of them (that is what CLONE_FS means). Rather than another
 * shared, reference-counted structure, it is written through to each
 * task in the group: there are at most 64 of them, chdir is rare, and
 * a task inside a system call cannot be preempted, so nothing can see
 * half of the change.
 */
void vfs_cwd_set(u32 ino, const char *path)
{
    struct task *t;
    int i;

    if (!current) {
        return;
    }
    for (i = 0; (t = task_nth(i)) != 0; i++) {
        if (t->tgid != current->tgid) {
            continue;
        }
        t->cwd_ino = ino;
        if (path) {
            strncpy(t->cwd_path, path, PATH_MAX - 1);
            t->cwd_path[PATH_MAX - 1] = '\0';
        }
    }
}

const char *vfs_cwd_path(void)
{
    return current ? current->cwd_path : "/";
}
