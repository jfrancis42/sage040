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
#include "vfs.h"
#include "errno.h"
#include "string.h"

#define DEV_PREFIX     "/dev/"
#define DEV_PREFIX_LEN 5

static struct fs_type *types;
static struct fs_type *mounted_fs;
static struct blockdev *mounted_dev;
static struct file files[OPEN_MAX];

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
    for (i = 0; i < OPEN_MAX; i++) {
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
    mounted_fs = 0;
    mounted_dev = 0;
    return 0;
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

static const char *strip_root(const char *path)
{
    return (path[0] == '/') ? path + 1 : path;
}

/* ---------------------------------------------------------------- */
/* Descriptors                                                       */
/* ---------------------------------------------------------------- */

struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= OPEN_MAX || !files[fd].used) {
        return 0;
    }
    return &files[fd];
}

static int fd_alloc(void)
{
    int i;

    for (i = 0; i < OPEN_MAX; i++) {
        if (!files[i].used) {
            memset(&files[i], 0, sizeof(files[i]));
            files[i].used = 1;
            return i;
        }
    }
    return -EMFILE;
}

int fd_bind(int fd, const struct file_ops *ops, void *priv, int flags)
{
    if (fd < 0 || fd >= OPEN_MAX || !ops) {
        return -EINVAL;
    }
    memset(&files[fd], 0, sizeof(files[fd]));
    files[fd].used = 1;
    files[fd].ops = ops;
    files[fd].priv = priv;
    files[fd].flags = flags;
    return fd;
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
        fd = fd_alloc();
        if (fd < 0) {
            return fd;
        }
        files[fd].ops = cd->ops;
        files[fd].priv = cd->priv;
        files[fd].flags = flags;
        return fd;
    }
    if (strncmp(path, DEV_PREFIX, DEV_PREFIX_LEN) == 0) {
        return -ENXIO;          /* under /dev, but no such device */
    }

    if (!mounted_fs) {
        return -ENODEV;
    }

    fd = fd_alloc();
    if (fd < 0) {
        return fd;
    }
    files[fd].flags = flags;
    err = mounted_fs->open(strip_root(path), flags, &files[fd]);
    if (err < 0) {
        files[fd].used = 0;
        return err;
    }
    return fd;
}

int fd_close(int fd)
{
    struct file *f = fd_get(fd);
    int err = 0;

    if (!f) {
        return -EBADF;
    }
    if (f->ops && f->ops->close) {
        err = f->ops->close(f);
    }
    f->used = 0;
    return err;
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
    return f->ops->write(f, buf, len);
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
    return mounted_fs->unlink(strip_root(path));
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
    return mounted_fs->rename(strip_root(from), strip_root(to));
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
    return mounted_fs->stat(strip_root(path), st);
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
