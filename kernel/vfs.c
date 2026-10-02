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
#include "events.h"
#include "reclock.h"
#include "pipe.h"
#include "errno.h"
#include "task.h"
#include "string.h"
#include "wait.h"
#include "signal.h"
#include "pty.h"
#include "ctty.h"
#include "procfs.h"
#include "timer.h"

#define DEV_PREFIX     "/dev/"
#define DEV_PREFIX_LEN 5

static struct fs_type *types;
static struct fs_type *mounted_fs;

/*
 * THE MOUNTS. "/" is whatever was mounted from the disk; /tmp and
 * /dev/shm are tmpfs (fs/tmpfs.c), in memory. There are no others, so
 * this is a routing function rather than a table: vfs_route() says
 * which filesystem a path is on, every call below that takes a path
 * asks it, and every open file remembers which one it came from
 * (file_fs, beside files[]). /proc is not a filesystem here; it is
 * answered before any of this is asked.
 *
 * A relative path is the working directory's filesystem's. The disk's
 * working directory is its own number for it (cwd_ino); tmpfs's is its
 * path only, so a relative name from inside tmpfs is made absolute here
 * -- as is a /tmp path with ".." in it, which may lead out.
 */
extern struct fs_type *tmpfs_fs(void);
extern int tmpfs_is_file(const struct file *f);

static int tmp_path(const char *p)
{
    return (strncmp(p, "/tmp", 4) == 0 && (p[4] == '/' || !p[4])) ||
           (strncmp(p, "/dev/shm", 8) == 0 && (p[8] == '/' || !p[8]));
}

static int tmp_cwd(void)
{
    return current && tmp_path(current->cwd_path);
}

static int has_dotdot(const char *p)
{
    for (; *p; p++) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || !p[2]) &&
            (p[-1] == '/')) {
            return 1;
        }
    }
    return 0;
}

static struct fs_type *vfs_route(const char **path, char *buf)
{
    const char *p = *path;
    int rel = p[0] != '/';

    if (!rel && !has_dotdot(p)) {
        return tmp_path(p) ? tmpfs_fs() : mounted_fs;
    }
    /*
     * Relative, or with ".." in it: where does it LAND? From "/",
     * "tmp/x" is tmpfs's; from /tmp, "../etc" is the disk's. Worked
     * out by name. The disk is still handed the path as it was when it
     * is the disk's, so its own ".." and symbolic links mean what they
     * always did.
     */
    if (vfs_abspath(p, buf, PATH_MAX) < 0) {
        buf[0] = '\0';         /* too long: nothing will be found */
        *path = buf;
        return tmp_cwd() ? tmpfs_fs() : mounted_fs;
    }
    if (tmp_path(buf)) {
        *path = buf;
        return tmpfs_fs();
    }
    if (rel && tmp_cwd()) {
        *path = buf;            /* out of tmpfs, onto the disk */
    }
    /*
     * And into /dev, which is not on the disk either: it is the device
     * registry, matched by its ABSOLUTE name. From /dev, "kbd0" was
     * handed to the disk as it stood, which has no such file -- so
     * `ls /dev` (sbase's ls chdirs into what it lists) said "lstat
     * kbd0: No such file or directory" for the first device it met.
     */
    if (rel && (strncmp(buf, "/dev/", 5) == 0 || strcmp(buf, "/dev") == 0)) {
        *path = buf;
    }
    return mounted_fs;
}

int vfs_tmp_owns(const char *path)
{
    char buf[PATH_MAX];
    const char *p = path;

    return vfs_route(&p, buf) != mounted_fs;
}

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
    u16 sr;

    /* The outermost holder, about to let go: between two calls, which
     * is where a journal may commit (fs/ext2.c, ext2_boundary). Still
     * holding the lock, so nothing else is inside the filesystem. */
    if (fs_depth == 1 && fs_owner == current && mounted_fs &&
        mounted_fs->boundary) {
        mounted_fs->boundary();
    }
    sr = irq_save();
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

/*
 * WHAT EACH OPEN FILE WAS OPENED AS, for /proc/<pid>/fd (procfs.c).
 *
 * An absolute path with "." and ".." taken out, recorded when a path is
 * opened; empty for what was never a path -- a pipe, a socket -- which
 * /proc names by kind instead. Beside `files` rather than in struct
 * file, which every driver sees: it is 256 KB, and nothing but /proc
 * reads it.
 *
 * What it is NOT is where the file is now. A rename leaves it naming
 * the old place, which is also what Linux does for a file renamed out
 * from under an open descriptor... almost: Linux follows the rename.
 * This does not, and says the name the file was opened by.
 */
static char file_paths[FILE_MAX][PATH_MAX];

/* Which filesystem each open file or directory is on: see vfs_route. */
static struct fs_type *file_fs[FILE_MAX];

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

/*
 * kjournald: once a second, between whatever calls are running, give the
 * filesystem its boundary. A journal commits only at one (ext2_boundary)
 * and only when a transaction is old enough, so without this a machine
 * that went quiet after a write would hold the change in memory until
 * the next call came along -- however long that was.
 */
static struct waitq flusher_wait;

void vfs_flusher(void)
{
    for (;;) {
        sleep_on_timeout(&flusher_wait, 1000);
        if (mounted_fs && mounted_fs->boundary) {
            fs_lock();
            fs_unlock();
        }
    }
}

/* Does the root volume record permission bits? FAT does not: its modes
 * are invented (syslinux.c, to_statx). */
int vfs_root_has_modes(void)
{
    return !mounted_fs || strcmp(mounted_fs->name, "fat16") != 0;
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
 * What stat() says a device is, by path or by descriptor: all of it
 * from the registry. The owner and mode used to be a constant --
 * S_IFCHR|0600, owner root -- which said every device belonged to root
 * whatever chown had been told; the inode and number were nothing at
 * all. See dev.h.
 */
static int perm_ok(const struct stat *st, int want);

static void dev_stat(const struct chardev *cd, struct stat *st)
{
    struct timeval tv;

    memset(st, 0, sizeof(*st));
    /* Made at boot, as Linux's devtmpfs nodes are; 0 read as 1970. */
    clock_get(&tv);
    st->st_mtime = (time_t)((u32)tv.tv_sec - timer_jiffies() / HZ);
    st->st_mode = (cd->block ? S_IFBLK : S_IFCHR) | (cd->mode & 07777);
    st->st_uid = cd->uid;
    st->st_gid = cd->gid;
    st->st_nlink = 1;
    st->st_dev = ST_DEV_DEVTMPFS;
    st->st_ino = cd->ino;
    st->st_rdev = cd->rdev;
}

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
 * IS THIS PATH A DIRECTORY IN /dev?
 *
 * /dev itself is, and so is any prefix a registered name sits under --
 * "pts", because the pseudo-terminals are called "pts/0" and so on.
 * There is no such directory anywhere: the whole of /dev is
 * synthesised from the device registry, and the names with a slash in
 * them are the only thing that makes it look like a tree.
 *
 * It has to answer, because the alternative is what this machine did
 * until now: stat("/dev") failed, and walk_ok() -- which checks search
 * permission on every directory along a path, and is skipped entirely
 * for root -- therefore returned ENOENT for any path under /dev the
 * moment a NON-ROOT process used one. The symptom was an interactive
 * `ssh` that could not start, because Dropbear chowns the pty to
 * whoever logged in and treats failing to do so as fatal; the error it
 * printed was "chown(/dev/pts/0, ...) failed: No such file or
 * directory", which names the pty and blames the wrong thing entirely.
 * Root never saw it, which is why every test in the tree missed it.
 */
static int dev_is_dir(const char *path)
{
    struct chardev *d;
    u32 n;

    if (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0) {
        return 1;
    }
    if (strncmp(path, DEV_PREFIX, DEV_PREFIX_LEN) != 0) {
        return 0;
    }
    path += DEV_PREFIX_LEN;
    n = (u32)strlen(path);
    if (n == 0) {
        return 1;
    }
    for (d = dev_first_char(); d; d = d->next) {
        if (strncmp(d->name, path, n) == 0 && d->name[n] == '/') {
            return 1;           /* something lives under this name */
        }
    }
    return 0;
}

/*
 * /proc.
 *
 * A path under /proc is procfs's before it is anybody's: its symbolic
 * links -- self, and a process's exe, cwd, root and fd/N -- are followed
 * by proc_lookup(), and what comes back is either a path procfs answers
 * itself or one somewhere else entirely (/proc/self/cwd/notes.txt is a
 * file on the volume), which the call then goes on with as though it
 * had been given that. So every call below that takes a path starts
 * with "if (proc_owns(path))" and one of these.
 *
 * Each is a separate function, not inline, so that the PATH_MAX buffer
 * it needs is on the stack only for a /proc path: the rest of this file
 * is already a kilobyte or two deep by the time the filesystem is
 * reached, and kernel stacks are 16 KB.
 */
#define NOINLINE __attribute__((noinline))

enum proc_op {
    PO_UNLINK, PO_RMDIR, PO_MKDIR, PO_CHDIR, PO_UTIME,
    PO_LINK, PO_SYMLINK, PO_RENAME
};

/* Resolve one path for an operation that changes something. 0 with
 * `out` a path elsewhere to carry on with; -errno to refuse. */
static int proc_modify_path(const char *path, int follow, int creates,
                            char *out)
{
    struct file *anon = 0;
    int r;

    if (!proc_owns(path)) {
        strcpy(out, path);
        return 0;
    }
    r = proc_lookup(path, follow, out, &anon);
    if (r == PROC_ELSEWHERE) {
        return 0;
    }
    if (r == PROC_ANON) {
        file_put(anon);
        return -EPERM;          /* a pipe has no name to change */
    }
    if (r == -ENOENT && creates) {
        /* A new name in a /proc directory: nothing can be added to
         * one, which is a permission question and not a missing file.
         * A missing directory above it is still ENOENT. */
        char parent[PATH_MAX];
        u32 cut = 0, i;
        int slash = 0;

        for (i = 0; path[i]; i++) {
            if (path[i] == '/') {
                cut = i;
                slash = 1;
            }
        }
        if (!slash) {
            strcpy(parent, ".");        /* a bare name: here */
        } else if (cut == 0) {
            strcpy(parent, "/");
        } else {
            memcpy(parent, path, cut);
            parent[cut] = '\0';
        }
        r = proc_lookup(parent, 1, out, &anon);
        if (r == PROC_ANON) {
            file_put(anon);
        }
        return r == PROC_HERE ? -EACCES : -ENOENT;
    }
    if (r < 0) {
        return r;
    }
    /* PROC_HERE: something /proc has, and nothing in /proc changes. */
    if (creates) {
        return -EEXIST;
    }
    return -EPERM;
}

static int NOINLINE proc_modify(enum proc_op op, const char *a,
                                const char *b, u32 x, u32 y)
{
    char pa[PATH_MAX], pb[PATH_MAX];
    int err;

    switch (op) {
    case PO_UNLINK:
    case PO_RMDIR:
        err = proc_modify_path(a, 0, 0, pa);
        if (err < 0) {
            return err;
        }
        return op == PO_UNLINK ? vfs_unlink(pa) : vfs_rmdir(pa);
    case PO_MKDIR:
        err = proc_modify_path(a, 0, 1, pa);
        return err < 0 ? err : vfs_mkdir(pa);
    case PO_CHDIR:
        if (proc_owns(a)) {
            struct file *anon = 0;
            int r = proc_lookup(a, 1, pa, &anon);

            if (r == PROC_ANON) {
                file_put(anon);
                return -ENOTDIR;
            }
            if (r == PROC_HERE) {
                struct stat st;

                /*
                 * Standing in /proc: the path is the whole of it. The
                 * filesystem's number for the working directory is
                 * left as it was and not consulted while the path is
                 * under /proc, when every relative name is /proc's
                 * (procfs.c, proc_owns).
                 */
                r = proc_stat(pa, &st);
                if (r < 0) {
                    return r;
                }
                if (!S_ISDIR(st.st_mode)) {
                    return -ENOTDIR;
                }
                r = vfs_may(pa, X_OK);
                if (r < 0) {
                    return r;
                }
                vfs_cwd_set(current->cwd_ino, pa);
                return 0;
            }
            if (r < 0) {
                return r;
            }
            return vfs_chdir(pa);
        }
        return vfs_chdir(a);
    case PO_UTIME:
        err = proc_modify_path(a, 1, 0, pa);
        return err < 0 ? err : vfs_utime(pa, x, y);
    case PO_LINK:
        err = proc_modify_path(a, 1, 0, pa);
        if (err == -EPERM) {
            err = 0;            /* linking TO /proc is refused below */
            strcpy(pa, a);
        }
        if (err < 0) {
            return err;
        }
        if (proc_owns(pa)) {
            return -EXDEV;      /* another filesystem, as it is on Linux */
        }
        err = proc_modify_path(b, 0, 1, pb);
        return err < 0 ? err : vfs_link(pa, pb);
    case PO_SYMLINK:
        /* The target is only a string; the new name is what matters. */
        err = proc_modify_path(b, 0, 1, pb);
        return err < 0 ? err : vfs_symlink(a, pb);
    case PO_RENAME:
        err = proc_modify_path(a, 0, 0, pa);
        if (err < 0) {
            return err;
        }
        err = proc_modify_path(b, 0, 1, pb);
        if (err == -EEXIST) {
            err = -EPERM;       /* over something /proc has */
        }
        return err < 0 ? err : vfs_rename(pa, pb);
    }
    return -EINVAL;
}

/* chmod and chown: of what a link leads to, as they are elsewhere. */
static int NOINLINE proc_setattr_path(const char *path, u32 mask, u32 mode,
                                      u32 uid, u32 gid)
{
    char buf[PATH_MAX];
    int err = proc_modify_path(path, 1, 0, buf);

    return err < 0 ? err : vfs_setattr(buf, mask, mode, uid, gid);
}

static int NOINLINE proc_stat_path(const char *path, struct stat *st,
                                   int follow)
{
    char buf[PATH_MAX];
    struct file *anon = 0;
    int r = proc_lookup(path, follow, buf, &anon);

    switch (r) {
    case PROC_HERE:
        return proc_stat(buf, st);
    case PROC_ELSEWHERE:
        return follow ? vfs_stat(buf, st) : vfs_lstat(buf, st);
    case PROC_ANON:
        r = vfs_file_stat(anon, st);
        file_put(anon);
        return r;
    }
    return r;
}

static int NOINLINE proc_readlink_path(const char *path, char *out, u32 size)
{
    char buf[PATH_MAX];
    struct file *anon = 0;
    int r = proc_lookup(path, 0, buf, &anon);

    switch (r) {
    case PROC_HERE:
        return proc_readlink(buf, out, size);
    case PROC_ELSEWHERE:
        return vfs_readlink(buf, out, size);
    case PROC_ANON:
        file_put(anon);
        return -EINVAL;
    }
    return r;
}

static int NOINLINE proc_open_path(const char *path, int flags)
{
    char buf[PATH_MAX];
    struct file *anon = 0;
    int r = proc_lookup(path, (flags & O_NOFOLLOW) ? 0 : 1, buf, &anon);

    switch (r) {
    case PROC_HERE:
        return proc_open(buf, flags);
    case PROC_ELSEWHERE:
        return fd_open(buf, flags);
    case PROC_ANON:
        return fd_install_file(anon, flags);
    }
    if (r == -ENOENT && (flags & O_CREAT)) {
        return -EACCES;         /* nothing is created in /proc */
    }
    return r;
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
            file_paths[i][0] = '\0';
            file_fs[i] = 0;
            return &files[i];
        }
    }
    return 0;
}

static int file_index(const struct file *f)
{
    return (f >= files && f < files + FILE_MAX) ? (int)(f - files) : -1;
}

static struct fs_type *fs_of(const struct file *f)
{
    int i = file_index(f);

    return (i >= 0 && file_fs[i]) ? file_fs[i] : mounted_fs;
}

/* A name that is not a path -- "anon_inode:[eventfd]" -- as it is. */
void vfs_file_set_name(struct file *f, const char *name)
{
    int i = file_index(f);

    if (i >= 0 && strlen(name) < PATH_MAX) {
        strcpy(file_paths[i], name);
    }
}

void vfs_file_set_path(struct file *f, const char *path)
{
    int i = file_index(f);

    if (i >= 0 && vfs_abspath(path, file_paths[i], PATH_MAX) < 0) {
        file_paths[i][0] = '\0';
    }
}

/*
 * The name /proc gives an open file: the path it was opened by, or, for
 * one that never had a path, its kind and a number that tells two of
 * them apart -- Linux's "pipe:[12345]", with the open file's slot for
 * the inode number a pipe here does not have.
 */
int vfs_file_name(struct file *f, char *out, u32 size)
{
    struct stat st;
    const char *kind = "anon_inode";
    char num[12];
    int i = file_index(f), n = 0;
    u32 v;

    if (i < 0 || size < 32) {
        return -EINVAL;
    }
    if (file_paths[i][0]) {
        if (strlen(file_paths[i]) >= size) {
            return -ENAMETOOLONG;
        }
        strcpy(out, file_paths[i]);
        return 0;
    }
    if (vfs_file_stat(f, &st) == 0) {
        if (S_ISFIFO(st.st_mode)) {
            kind = "pipe";
        } else if (S_ISSOCK(st.st_mode)) {
            kind = "socket";
        }
    }
    v = st.st_ino ? st.st_ino : (u32)i + 1;
    do {
        num[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    strcpy(out, kind);
    i = (int)strlen(out);
    out[i++] = ':';
    out[i++] = '[';
    while (n) {
        out[i++] = num[--n];
    }
    out[i++] = ']';
    out[i] = '\0';
    return 0;
}

/*
 * An absolute path with every "." and ".." taken out and every run of
 * slashes made one: what `path` means from the caller's working
 * directory, written out. Symbolic links are NOT resolved -- that needs
 * the filesystem, and a name is what this is for.
 */
int vfs_abspath(const char *path, char *out, u32 size)
{
    char tmp[PATH_MAX];
    u32 n = 0, i = 0;

    if (!path || !*path) {
        return -ENOENT;
    }
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(tmp)) {
            return -ENAMETOOLONG;
        }
        strcpy(tmp, path);
    } else {
        const char *cwd = vfs_cwd_path();
        u32 c = (u32)strlen(cwd);

        if (c + 1 + strlen(path) >= sizeof(tmp)) {
            return -ENAMETOOLONG;
        }
        strcpy(tmp, cwd);
        tmp[c] = '/';
        strcpy(tmp + c + 1, path);
    }

    /* One component at a time, into out, which always starts with "/". */
    if (size < 2) {
        return -ENAMETOOLONG;
    }
    out[n++] = '/';
    while (tmp[i]) {
        u32 start, len;

        while (tmp[i] == '/') {
            i++;
        }
        start = i;
        while (tmp[i] && tmp[i] != '/') {
            i++;
        }
        len = i - start;
        if (len == 0 || (len == 1 && tmp[start] == '.')) {
            continue;
        }
        if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
            /* Back to the slash before the last component; the root's
             * parent is the root. */
            while (n > 1 && out[n - 1] != '/') {
                n--;
            }
            if (n > 1) {
                n--;            /* and the slash before it */
            }
            continue;
        }
        if (n + len + 1 >= size) {
            return -ENAMETOOLONG;
        }
        if (n > 1) {
            out[n++] = '/';
        }
        memcpy(out + n, tmp + start, len);
        n += len;
    }
    out[n] = '\0';
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
    events_file_gone(f);    /* no epoll goes on watching it */
    reclock_file_gone(f);   /* nor holds an OFD lock */
    if (inotify_watching) {
        inotify_file(f, (f->flags & O_ACCMODE) == O_RDONLY ? IN_CLOSE_NOWRITE
                                                           : IN_CLOSE_WRITE);
    }
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

u32 flock_key(struct file *f)
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
    return st.st_ino ? vfs_file_key(&st) : (u32)f->priv ^ 0x80000000UL;
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
    {
        /* A terminal bound at boot, or by a getty, is still a device
         * with a name: /proc/<pid>/fd/0 says which. */
        struct chardev *cd = dev_char_for(f);

        if (cd) {
            char name[PATH_MAX];

            strcpy(name, DEV_PREFIX);
            strncpy(name + DEV_PREFIX_LEN, cd->name,
                    PATH_MAX - DEV_PREFIX_LEN - 1);
            name[PATH_MAX - 1] = '\0';
            vfs_file_set_path(f, name);
        }
    }

    if (current->files->fd[fd]) {
        file_put(current->files->fd[fd]);
    }
    current->files->fd[fd] = f;
    current->files->flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    return fd;
}

/*
 * A new descriptor for an open file that already exists, taking the
 * caller's reference to it: how /proc/<pid>/fd/N opens something that
 * has no path to open again (a pipe, a socket). The two descriptors
 * then share one open file, position and all, where Linux would give a
 * new open file on the same pipe -- the difference is only visible to
 * a program that seeks, and a pipe cannot be sought.
 */
int fd_install_file(struct file *f, int flags)
{
    int fd = fd_alloc();

    if (fd < 0) {
        file_put(f);
        return fd;
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

/*
 * An open directory is described as stat would describe it by its path
 * as it is now: its own mode, owner, device and inode number. This used
 * to make up a mode of 0755 and give the filesystem's handle as the
 * inode number -- which, with more than one volume, carries the volume
 * in its top byte and matches nothing stat says.
 */
static int dir_fstat(struct file *f, struct stat *st)
{
    struct fs_type *fs = file_fs[file_index(f)];
    u32 ino = dir_ino_of(f);
    char path[PATH_MAX];
    int r = -ENOSYS;

    if (fs && fs->dir_path && fs->stat) {
        fs_lock();
        r = fs->dir_path(ino, path, sizeof(path));
        if (r == 0) {
            r = fs->stat(path, st);
        }
        fs_unlock();
    }
    if (r != 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_ino = ino ? ino : 1;
    }
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

static int dir_open(struct fs_type *fs, const char *path, int flags)
{
    u32 ino;
    int err;

    if (!fs->dir_ino || !fs->readdir_in) {
        return -ENOSYS;
    }
    fs_lock();
    err = fs->dir_ino(path, &ino);
    fs_unlock();
    if (err < 0) {
        return err;
    }
    err = fd_install(&dir_ops, (void *)(ino + 1), flags & ~O_ACCMODE);
    if (err >= 0) {
        struct file *f = fd_get(err);

        vfs_file_set_path(f, path);
        file_fs[file_index(f)] = fs;
    }
    return err;
}

int vfs_is_dir_file(struct file *f)
{
    return f && (f->ops == &dir_ops || proc_is_dir_file(f));
}

/* Where an open directory is now -- worked out, not remembered, so it
 * follows the directory through a rename. */
int vfs_dir_path(struct file *f, char *out, u32 size)
{
    int r;

    if (!vfs_is_dir_file(f)) {
        return -ENOTDIR;
    }
    if (proc_is_dir_file(f)) {
        return proc_dir_path(f, out, size);
    }
    {
        struct fs_type *fs = fs_of(f);

        if (!fs || !fs->dir_path) {
            return -ENOSYS;
        }
        fs_lock();
        r = fs->dir_path(dir_ino_of(f), out, size);
        fs_unlock();
        /* tmpfs names /dev/shm "/shm" among its own: say it as the
         * system does. */
        if (r == 0 && fs != mounted_fs && strncmp(out, "/shm", 4) == 0 &&
            (out[4] == '/' || !out[4])) {
            u32 n = (u32)strlen(out);

            if (n + 4 >= size) {
                return -ENAMETOOLONG;
            }
            memmove(out + 4, out, n + 1);
            memcpy(out, "/dev", 4);
        }
        return r;
    }
}

static int vfs_utime_raw(const char *path, u32 mtime, u32 atime)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];
    int r;
    fs = vfs_route(&path, rbuf);

    if (proc_owns(path)) {
        return proc_modify(PO_UTIME, path, 0, mtime, atime);
    }
    if (fs == mounted_fs && resolve_dev(path)) {
        return 0;               /* a device has no time to keep */
    }
    if (!fs || !fs->utime) {
        return -ENOSYS;
    }
    fs_lock();
    r = fs->utime(path, mtime, atime);
    fs_unlock();
    return r;
}

static int vfs_futime_raw(int fd, u32 mtime, u32 atime)
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
    {
        struct fs_type *fs = fs_of(f);

        if (!fs || !fs->futime) {
            return -ENOSYS;
        }
        fs_lock();
        r = fs->futime(f, mtime, atime);
        fs_unlock();
        return r;
    }
}

int vfs_fchdir(int fd)
{
    struct file *f = fd_get(fd);
    char path[PATH_MAX];
    int r;

    if (!f) {
        return -EBADF;
    }
    if (proc_is_dir_file(f) || (vfs_is_dir_file(f) && fs_of(f) != mounted_fs)) {
        r = vfs_dir_path(f, path, sizeof(path));
        if (r == 0) {
            vfs_cwd_set(current->cwd_ino, path);    /* see vfs_chdir */
        }
        return r;
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
    if (proc_is_dir_file(f)) {
        return proc_getdents64(f, buf, len);
    }
    ino = dir_ino_of(f);
    /* FAT's root, and only the disk's: tmpfs lists its own dots. */
    synth = (ino == 0 && fs_of(f) == mounted_fs) ? 2 : 0;

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
            err = fs_of(f)->readdir_in(ino, idx - synth, &d);
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
        r->d_type = S_ISDIR(d.d_mode) ? DT_DIR :
                    S_ISLNK(d.d_mode) ? DT_LNK : DT_REG;
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
    return fd_open_mode(path, flags, VFS_NO_MODE);
}

/*
 * open(2)'s MODE, applied as it says: to a file this call creates, less
 * the caller's umask. It used to be ignored -- a filesystem created
 * every file 0644 whatever was asked -- so a program that made a
 * private file with 0600 got one anybody could read. Set through the
 * filesystem's own fsetattr once the file exists, which is the one path
 * every filesystem already has.
 */
static int fd_open_mode_raw(const char *path, int flags, u32 mode)
{
    struct chardev *cd = 0;
    int created = 0;
    struct fs_type *fs;
    char rbuf[PATH_MAX];
    int fd, err;

    if (!path || !*path) {
        return -EINVAL;
    }
    if (strlen(path) > PATH_MAX) {
        return -ENAMETOOLONG;
    }
    if (proc_owns(path)) {
        return proc_open_path(path, flags);
    }
    fs = vfs_route(&path, rbuf);

    /* A device node is not a file on the volume and does not need one to
     * be mounted, which is what lets the console work before the disk
     * has been looked at. (/dev/shm is not a device: tmpfs.) */
    if (fs == mounted_fs) {
        cd = resolve_dev(path);
    }
    /*
     * A DEVICE'S MODE IS A RULE, as a file's is: checked against the
     * opener like any other permission (perm_ok). It used to be recorded
     * and shown by ls and never consulted, so any user could open anyone
     * else's terminal. /dev/tty is checked as itself -- 0666, it names
     * the opener's own terminal -- not as the terminal it leads to.
     */
    if (cd) {
        struct stat dst;
        int acc = flags & O_ACCMODE;

        dev_stat(cd, &dst);
        err = perm_ok(&dst, acc == O_RDONLY ? R_OK :
                            acc == O_WRONLY ? W_OK : R_OK | W_OK);
        if (err < 0) {
            return err;
        }
    }
    if (cd) {
        int fd = fd_install(cd->ops, cd->priv, flags);
        struct file *f;

        if (fd < 0) {
            return fd;
        }
        f = fd_get(fd);
        if (f) {
            vfs_file_set_path(f, path);
        }
        /*
         * /dev/ptmx is not a device to open: opening it ALLOCATES a
         * pseudo-terminal pair and gives back the master of it, so the
         * descriptor just installed is pointed somewhere else entirely.
         * A slave, /dev/pts/N, is an ordinary device open -- but the
         * pty has to be told, so that it knows when the program on the
         * terminal has gone.
         */
        /*
         * /dev/tty is the controlling terminal of whoever opens it --
         * the screen, the serial line or a pty -- so the descriptor is
         * pointed at that device. With no controlling terminal there is
         * nothing to open: ENXIO, as on Linux.
         */
        if (f && strcmp(cd->name, "tty") == 0) {
            struct chardev *ct = ctty_device();

            if (!ct) {
                fd_close(fd);
                return -ENXIO;
            }
            f->ops = ct->ops;
            f->priv = ct->priv;
            cd = ct;
        }
        if (f && strcmp(cd->name, "ptmx") == 0) {
            int err = pty_open_master(f);

            if (err < 0) {
                fd_close(fd);
                return err;
            }
        } else if (f && strncmp(cd->name, "pts/", 4) == 0) {
            pty_slave_opened(cd->priv);
        }
        if (f) {
            ctty_opened(cd, flags);     /* a session leader may gain it */
        }
        return fd;
    }
    if (fs == mounted_fs && strncmp(path, DEV_PREFIX, DEV_PREFIX_LEN) == 0) {
        return -ENOENT;         /* under /dev, no such device: as Linux */
    }

    if (!fs) {
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
        exists = fs->stat && fs->stat(path, &st) == 0;
        fs_unlock();

        if (exists && S_ISDIR(st.st_mode)) {
            if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT)) {
                return -EISDIR;
            }
            return dir_open(fs, path, flags);
        }
        /* A FIFO is a name for a pipe: opening it joins that pipe
         * (pipe.c), once the caller may. */
        if (exists && S_ISFIFO(st.st_mode)) {
            int acc = flags & O_ACCMODE, fd;
            struct file *ff;

            if ((flags & O_CREAT) && (flags & O_EXCL)) {
                return -EEXIST;
            }
            err = vfs_may(path, (acc == O_RDONLY ? R_OK :
                                 acc == O_WRONLY ? W_OK : R_OK | W_OK));
            if (err < 0) {
                return err;
            }
            fd = fifo_open(fs, st.st_ino, st.st_dev, flags & ~(O_CREAT | O_EXCL | O_TRUNC));
            if (fd >= 0 && (ff = fd_get(fd)) != 0) {
                vfs_file_set_path(ff, path);
            }
            return fd;
        }
        if (flags & O_DIRECTORY) {
            return exists ? -ENOTDIR : -ENOENT;
        }
        if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
            return -EEXIST;
        }
        created = !exists && (flags & O_CREAT);
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
        err = fs->open(path, flags & ~O_CLOEXEC, f);
        fs_unlock();
        f->fs = 1;
        file_fs[file_index(f)] = fs;
        if (err < 0) {
            f->used = 0;
            return err;
        }
        vfs_file_set_path(f, path);
        if (created && mode != VFS_NO_MODE && fs->fsetattr && current) {
            fs_lock();
            fs->fsetattr(f, ATTR_MODE, (mode & ~current->umask) & 07777, 0, 0);
            fs_unlock();
        }
        current->files->fd[fd] = f;
        current->files->flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    }
    if (flags & O_TRUNC) {
        /* Emptied: its shared pages read as zero now, the rest are
         * forgotten. */
        textcache_after_change(fd_get(fd));
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
    reclock_closed(f, current->tgid);   /* POSIX: ANY close drops them */
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
    case F_DUPFD_CLOEXEC: {
        int n = dup_from(fd, (int)arg);

        if (n >= 0) {
            current->files->flags[n] = FD_CLOEXEC;
        }
        return n;
    }
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
        reclock_closed(current->files->fd[newfd], current->tgid);
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
            reclock_closed(t->files->fd[i], t->tgid);
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

static s32 fd_read_raw(int fd, void *buf, u32 len)
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
    /* A read sees what a shared mapping has written: textcache.c. */
    textcache_before_io(f);
    {
        s32 r;

        fs_lock();
        r = f->ops->read(f, buf, len);
        fs_unlock();
        return r;
    }
}

static s32 fd_write_raw(int fd, const void *buf, u32 len)
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

        /* What shared mappings hold goes to the file before the write
         * lands on top of it, and they read the result after. */
        if (f->fs) {
            textcache_before_io(f);
            fs_lock();
        }
        n = f->ops->write(f, buf, len);
        if (f->fs) {
            fs_unlock();
        }

        /* A file that changes is not what textcache.c holds of it any
         * more: its private pages are forgotten and its shared ones read
         * again. The same after a truncate and an O_TRUNC open; an
         * unlink and a rename forget too. */
        if (n > 0 && f->fs) {
            textcache_after_change(f);
        }
        return n;
    }
}

/*
 * Read or write at `off` without moving the file's position -- which
 * belongs to every descriptor sharing the open file -- or appending
 * because of O_APPEND: what textcache.c needs to fill a page and write
 * one back. Under the filesystem lock for the whole of it, so no other
 * task sees the position in between.
 */
static s32 file_pio(struct file *f, u32 off, void *buf, u32 len, int write)
{
    u32 pos;
    int flags;
    s32 n;

    if (!f || !f->ops || !(write ? (void *)f->ops->write
                                 : (void *)f->ops->read)) {
        return -EINVAL;
    }
    if (f->fs) {
        fs_lock();
    }
    pos = f->pos;
    flags = f->flags;
    f->flags &= ~O_APPEND;
    f->pos = off;
    n = write ? f->ops->write(f, buf, len) : f->ops->read(f, buf, len);
    f->pos = pos;
    f->flags = flags;
    if (f->fs) {
        fs_unlock();
    }
    return n;
}

s32 vfs_file_pread(struct file *f, u32 off, void *buf, u32 len)
{
    return file_pio(f, off, buf, len, 0);
}

s32 vfs_file_pwrite(struct file *f, u32 off, const void *buf, u32 len)
{
    return file_pio(f, off, (void *)buf, len, 1);
}

/*
 * pread and pwrite: read() and write() at an offset, leaving the
 * position alone -- Linux's rules for which files may (anything that
 * can seek; ESPIPE otherwise) and the same coherence with shared
 * mappings as read() and write() have.
 */
s32 fd_pread(int fd, void *buf, u32 len, u32 off)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        return -EBADF;
    }
    if (!f->ops->lseek || !f->ops->read || vfs_is_dir_file(f)) {
        return vfs_is_dir_file(f) ? -EISDIR : -ESPIPE;
    }
    if (f->fs) {
        textcache_before_io(f);
    }
    return file_pio(f, off, buf, len, 0);
}

static s32 fd_pwrite_raw(int fd, const void *buf, u32 len, u32 off)
{
    struct file *f = fd_get(fd);
    s32 n;

    if (!f) {
        return -EBADF;
    }
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        return -EBADF;
    }
    if (!f->ops->lseek || !f->ops->write) {
        return -ESPIPE;
    }
    if (is_swapfile(fd)) {
        return -ETXTBSY;
    }
    if (f->fs) {
        textcache_before_io(f);
    }
    n = file_pio(f, off, (void *)buf, len, 1);
    if (n > 0 && f->fs) {
        textcache_after_change(f);
    }
    return n;
}

/* fsync: this file's shared pages, then everything. */
int vfs_fsync(int fd)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    textcache_sync_file(f);
    return vfs_sync();
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
    return vfs_mkdir_mode(path, VFS_NO_MODE);
}

/* mkdir(2)'s mode, less the umask, as open's is (fd_open_mode). */
static int vfs_mkdir_mode_raw(const char *path, u32 mode)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];

    if (proc_owns(path)) {
        return proc_modify(PO_MKDIR, path, 0, 0, 0);
    }
    fs = vfs_route(&path, rbuf);
    if (!fs || !fs->mkdir) {
        return -ENOSYS;
    }
    {
        struct stat st;
        int r = vfs_may_parent(path, X_OK), there;

        /*
         * EEXIST before EACCES, as Linux answers: whoever may search the
         * parent learns that the name is taken, whether or not they may
         * write there. `mkdir -p /tmp/x` relies on it -- it makes /tmp
         * first and carries on at EEXIST, and a user with no write
         * permission on / was told EACCES and stopped.
         */
        if (r < 0) {
            return r;
        }
        fs_lock();
        there = (fs->lstat ? fs->lstat(path, &st) : fs->stat(path, &st)) == 0;
        fs_unlock();
        if (there) {
            return -EEXIST;
        }
        r = vfs_may_parent(path, W_OK | X_OK);
        if (r < 0) {
            return r;
        }
        fs_lock();
        r = fs->mkdir(path);
        if (r == 0 && mode != VFS_NO_MODE && fs->setattr && current) {
            /* As Linux: mkdir's mode gives the permission bits and the
             * sticky bit, never setuid or setgid -- but a setgid the new
             * directory INHERITED from its parent stays. */
            u32 keep = 0;

            if (fs->stat(path, &st) == 0) {
                keep = st.st_mode & S_ISGID;
            }
            fs->setattr(path, ATTR_MODE,
                        ((mode & ~current->umask) & 01777) | keep, 0, 0);
        }
        fs_unlock();
        return r;
    }
}

static int vfs_rmdir_raw(const char *path)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];

    if (proc_owns(path)) {
        return proc_modify(PO_RMDIR, path, 0, 0, 0);
    }
    fs = vfs_route(&path, rbuf);
    if (!fs || !fs->rmdir) {
        return -ENOSYS;
    }
    {
        int r = vfs_may_parent(path, W_OK | X_OK);

        if (r < 0) {
            return r;
        }
        fs_lock();
        r = fs->rmdir(path);
        fs_unlock();
        return r;
    }
}

/*
 * Into tmpfs: the working directory is its path alone, as it is in /proc
 * -- the disk's number for it (cwd_ino) stays as it was and is not
 * consulted while cwd_path is in tmpfs, because every relative name is
 * routed there first (vfs_route). `path` is absolute.
 */
static int tmp_chdir(const char *path)
{
    struct stat st;
    int r = vfs_stat(path, &st);

    if (r < 0) {
        return r;
    }
    if (!S_ISDIR(st.st_mode)) {
        return -ENOTDIR;
    }
    r = vfs_may(path, X_OK);
    if (r < 0) {
        return r;
    }
    vfs_cwd_set(current->cwd_ino, path);
    return 0;
}

static int vfs_chdir_disk(const char *path);

/*
 * "dir/" is "dir" to mkdir and rmdir, as on Linux. The filesystems take
 * the last component as the name to make, and a trailing slash made
 * that name empty: mkdir("/x/") said ENOENT. git's `init` copies its
 * template directories with exactly that, and every repository it
 * tried to make on the machine stopped at "/r/.git/hooks/: No such
 * file or directory". In place, in a buffer the caller owns.
 */
void vfs_trim_slashes(char *path)
{
    u32 n = (u32)strlen(path);

    while (n > 1 && path[n - 1] == '/') {
        path[--n] = '\0';
    }
}

int vfs_chdir(const char *path)
{
    if (proc_owns(path)) {
        return proc_modify(PO_CHDIR, path, 0, 0, 0);
    }
    {
        char rbuf[PATH_MAX];

        /* Routed first: from inside tmpfs a relative name comes back
         * absolute, so the disk is never asked about ".." from the
         * working directory it last had. */
        if (vfs_route(&path, rbuf) != mounted_fs) {
            return tmp_chdir(path);
        }
        /* ".." led out, to the disk -- or into /proc, which is asked
         * by its absolute name. This was a recursive call with a copy
         * of rbuf: another 2 KB of kernel stack, on the path that
         * overflowed it. rbuf is absolute and lives to the end. */
        if (path == rbuf && proc_owns(path)) {
            return proc_modify(PO_CHDIR, path, 0, 0, 0);
        }
        return vfs_chdir_disk(path);
    }
}

static int vfs_chdir_disk(const char *path)
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
    if (proc_cwd() || tmp_cwd()) {
        return current->cwd_path;       /* the filesystem's is stale */
    }
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

static int vfs_unlink_raw(const char *path)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];

    if (proc_owns(path)) {
        return proc_modify(PO_UNLINK, path, 0, 0, 0);
    }
    fs = vfs_route(&path, rbuf);
    if (fs == mounted_fs && resolve_dev(path)) {
        return -EPERM;          /* device nodes are not files */
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->unlink) {
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
        r = fs->unlink(path);
        fs_unlock();
        return r;
    }
}

/*
 * link(2). The permission is on the DIRECTORY the new name goes in,
 * not on the file: making a second name for a file does not change the
 * file, and you need no right over it beyond being able to reach it.
 */
static int walk_ok(const char *path);

static int vfs_link_raw(const char *from, const char *to)
{
    struct fs_type *fs, *fs2;
    char rbuf[PATH_MAX], rbuf2[PATH_MAX];
    int err;

    if (proc_owns(from) || proc_owns(to)) {
        return proc_modify(PO_LINK, from, to, 0, 0);
    }
    fs = vfs_route(&from, rbuf);
    fs2 = vfs_route(&to, rbuf2);
    if (fs != fs2) {
        return -EXDEV;          /* a link cannot cross filesystems */
    }
    if (fs == mounted_fs && (resolve_dev(from) || resolve_dev(to))) {
        return -EPERM;          /* device nodes are not files */
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->link) {
        return -EPERM;          /* what link(2) says for a volume that
                                 * has no such thing */
    }
    /* Reachable -- search permission above it -- and there, as a NAME:
     * link(2) does not follow a symbolic link (Linux's linkat without
     * AT_SYMLINK_FOLLOW), it gives the link itself a second name. Asking
     * vfs_may, which stats THROUGH a link, refused every dangling one. */
    err = walk_ok(from);
    if (err < 0) {
        return err;
    }
    {
        struct stat lst;

        err = vfs_lstat(from, &lst);
        if (err < 0) {
            return err;
        }
    }
    err = vfs_may_parent(to, W_OK | X_OK);
    if (err < 0) {
        return err;
    }
    {
        int r;

        fs_lock();
        r = fs->link(from, to);
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
    struct fs_type *fs;
    char rbuf[PATH_MAX];

    if (proc_owns(path)) {
        return proc_stat_path(path, st, 0);
    }
    fs = vfs_route(&path, rbuf);
    if (!fs) {
        return -ENODEV;
    }
    /* A device, or a directory of them, is no link: lstat is stat. It
     * went to the disk, which has no /dev, so lstat("/dev/null") failed
     * -- and with it every `ls -l` of a device. */
    if (!fs->lstat || (fs == mounted_fs && (resolve_dev(path) || dev_is_dir(path)))) {
        return vfs_stat(path, st);
    }
    {
        int r;

        fs_lock();
        r = fs->lstat(path, st);
        fs_unlock();
        return r;
    }
}

static int vfs_symlink_raw(const char *target, const char *linkpath)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];
    int err;
    fs = vfs_route(&linkpath, rbuf);

    if (proc_owns(linkpath)) {
        return proc_modify(PO_SYMLINK, target, linkpath, 0, 0);
    }
    if (fs == mounted_fs && resolve_dev(linkpath)) {
        return -EEXIST;
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->symlink) {
        return -EPERM;
    }
    err = vfs_may_parent(linkpath, W_OK | X_OK);
    if (err < 0) {
        return err;
    }
    {
        int r;

        fs_lock();
        r = fs->symlink(target, linkpath);
        fs_unlock();
        return r;
    }
}

/*
 * mknod(2): a FIFO, or a plain empty file. A device node is refused:
 * devices here are names under /dev (resolve_dev), not inodes, so a
 * device node on the disk would name nothing -- EPERM, as for a caller
 * without CAP_MKNOD on Linux. The permission bits lose the umask.
 */
static int vfs_mknod_raw(const char *path, u32 mode)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];
    const char *orig = path;
    u32 perm = mode & 07777;
    int err;

    if (current) {
        perm &= ~current->umask;
    }
    switch (mode & S_IFMT) {
    case 0:
    case S_IFREG:
        err = fd_open_mode(orig, O_CREAT | O_EXCL | O_WRONLY, mode & 07777);
        return err < 0 ? err : fd_close(err);
    case S_IFIFO:
        break;
    case S_IFDIR:
    case S_IFCHR:
    case S_IFBLK:
    case S_IFSOCK:
        return -EPERM;
    default:
        return -EINVAL;
    }
    fs = vfs_route(&path, rbuf);
    if (proc_owns(path) || (fs == mounted_fs && resolve_dev(path))) {
        return -EEXIST;
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->mknod) {
        return -EPERM;
    }
    err = vfs_may_parent(path, W_OK | X_OK);
    if (err < 0) {
        return err;
    }
    fs_lock();
    err = fs->mknod(path, S_IFIFO | perm);
    fs_unlock();
    return err;
}

/*
 * readlink(2). Needs only to REACH the link -- no read permission on
 * the link itself, whose mode is 0777 and means nothing, and none on
 * the target, which may not even exist.
 */
int vfs_readlink(const char *path, char *out, u32 size)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];
    int err;
    fs = vfs_route(&path, rbuf);

    if (proc_owns(path)) {
        return proc_readlink_path(path, out, size);
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->readlink) {
        return -EINVAL;         /* what readlink says for a non-link */
    }
    err = vfs_may_parent(path, X_OK);
    if (err < 0) {
        return err;
    }
    {
        int r;

        fs_lock();
        r = fs->readlink(path, out, size);
        fs_unlock();
        return r;
    }
}

static int vfs_rename_raw(const char *from, const char *to)
{
    struct fs_type *fs, *fs2;
    char rbuf[PATH_MAX], rbuf2[PATH_MAX];

    if (proc_owns(from) || proc_owns(to)) {
        return proc_modify(PO_RENAME, from, to, 0, 0);
    }
    fs = vfs_route(&from, rbuf);
    fs2 = vfs_route(&to, rbuf2);
    if (fs != fs2) {
        return -EXDEV;          /* mv copies, when it is told this */
    }
    if (fs == mounted_fs && (resolve_dev(from) || resolve_dev(to))) {
        return -EPERM;
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->rename) {
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
        r = fs->rename(from, to);
        fs_unlock();
        return r;
    }
}

int vfs_stat(const char *path, struct stat *st)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];
    struct chardev *cd;
    fs = vfs_route(&path, rbuf);

    if (proc_owns(path)) {
        return proc_stat_path(path, st, 1);
    }
    cd = fs == mounted_fs ? resolve_dev(path) : 0;

    if (!cd && fs == mounted_fs && dev_is_dir(path)) {
        /* A synthesised directory: /dev, and /dev/pts. Searchable and
         * readable by everybody, writable by nobody -- nothing can be
         * created in it, because what is in it is the registry. */
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        st->st_dev = ST_DEV_DEVTMPFS;
        st->st_ino = strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0
                     ? DEV_INO_ROOT : DEV_INO_SUBDIR;
        return 0;
    }
    if (cd) {
        dev_stat(cd, st);
        return 0;
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->stat) {
        return -ENOSYS;
    }
    {
        int r;

        fs_lock();
        r = fs->stat(path, st);
        fs_unlock();
        return r;
    }
}

static int vfs_ftruncate_raw(int fd, u32 len)
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
    /* What shared mappings hold goes to the file first, so that what
     * the truncate keeps is theirs; after, they read the file again. */
    textcache_before_io(f);
    {
        int r;

        if (f->fs) {
            fs_lock();
        }
        r = f->ops->truncate(f, len);
        if (f->fs) {
            fs_unlock();
        }
        textcache_after_change(f);
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
    if (!fs_of(fd_get(fd)) || !fs_of(fd_get(fd))->bmap || !f->ops || !f->ops->fstat ||
        vfs_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        return -EINVAL;
    }
    {
        int r;

        fs_lock();
        r = fs_of(fd_get(fd))->bmap(f, off, lba, dev);
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
    if (proc_cwd() || tmp_cwd()) {
        /* The old getdents reads the filesystem's working directory,
         * which is not where the caller is. getdents64 lists /proc. */
        return -EOPNOTSUPP;
    }
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
    return vfs_file_stat(f, st);
}

/* The same, of an open file rather than a descriptor: what /proc needs
 * to describe another task's. */
int vfs_file_stat(struct file *f, struct stat *st)
{
    struct chardev *cd;

    memset(st, 0, sizeof(*st));

    /*
     * A DESCRIPTOR ON A DEVICE IS DESCRIBED BY THE REGISTRY, as the
     * device's path is: the same inode, number, owner and mode either
     * way. The drivers' own fstat ops only ever said S_IFCHR, so fstat
     * of the console and stat of /dev/null came out identical, and a
     * program comparing the two (GNU cmp does, to skip writing to
     * /dev/null) concluded its stdout was /dev/null.
     */
    cd = dev_char_for(f);
    if (cd) {
        dev_stat(cd, st);
        return 0;
    }

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
    int has_slash = 0;
    int err;

    for (i = 0; path[i]; i++) {
        if (path[i] == '/') {
            cut = i;
            has_slash = 1;
        }
    }
    if (!has_slash) {
        /*
         * A bare name like "abc.txt" -- its parent is the CURRENT
         * directory, not the root. Getting this wrong checked write
         * permission on "/" for every relative creation, so a user
         * could make files in its own home only by absolute path.
         */
        dir[0] = '.';
        dir[1] = '\0';
    } else if (cut == 0) {
        /* "/abc" at the top -- the parent is the root. */
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

/*
 * chown or chmod a DEVICE, whose owner and mode live in the registry
 * rather than on the volume. Shares may_setattr() with the filesystem
 * path, so "only root gives a file away" and "an ownership change
 * clears set-user-id" mean the same thing here as anywhere.
 */
static int dev_setattr(struct chardev *cd, u32 mask, u32 mode,
                       u32 uid, u32 gid)
{
    struct stat st;
    int err;

    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFCHR | (cd->mode & 07777);
    st.st_uid = cd->uid;
    st.st_gid = cd->gid;

    err = may_setattr(&st, &mask, &mode, uid, gid);
    if (err < 0) {
        return err;
    }
    if (mask & ATTR_UID) {
        cd->uid = uid;
    }
    if (mask & ATTR_GID) {
        cd->gid = gid;
    }
    if (mask & ATTR_MODE) {
        cd->mode = mode & 07777;
    }
    return 0;
}

static int vfs_setattr_raw(const char *path, u32 mask, u32 mode, u32 uid, u32 gid)
{
    struct fs_type *fs;
    char rbuf[PATH_MAX];
    struct chardev *cd;
    struct stat st;
    int err;
    fs = vfs_route(&path, rbuf);

    if (proc_owns(path)) {
        return proc_setattr_path(path, mask, mode, uid, gid);
    }
    err = walk_ok(path);
    if (err < 0) {
        return err;
    }
    cd = fs == mounted_fs ? resolve_dev(path) : 0;
    if (cd) {
        return dev_setattr(cd, mask, mode, uid, gid);
    }
    if (fs == mounted_fs && strncmp(path, DEV_PREFIX, DEV_PREFIX_LEN) == 0) {
        return -ENOENT;         /* under /dev, no such device: as Linux */
    }
    if (!fs) {
        return -ENODEV;
    }
    if (!fs->setattr) {
        return -ENOSYS;         /* a volume with nowhere to put it */
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
        r = fs->setattr(path, mask, mode, uid, gid);
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
static int vfs_fsetattr_raw(int fd, u32 mask, u32 mode, u32 uid, u32 gid)
{
    struct stat st;
    struct chardev *cd;
    int err;

    /* fchmod on a terminal is ordinary -- and a descriptor is the only
     * handle a program has on a pty it was given rather than opened. */
    cd = dev_char_for(fd_get(fd));
    if (cd) {
        return dev_setattr(cd, mask, mode, uid, gid);
    }
    if (fd_get(fd) && !fd_get(fd)->fs) {
        /* A pipe, a socket, a file in /proc: nowhere to keep a mode.
         * It used to be handed to the filesystem, which took it for
         * one of its own files. */
        return -EPERM;
    }
    if (!fs_of(fd_get(fd))) {
        return -ENODEV;
    }
    if (!fs_of(fd_get(fd))->fsetattr) {
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
        r = fs_of(fd_get(fd))->fsetattr(f, mask, mode, uid, gid);
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

/*
 * statfs(2): the volume `path` is on. Null means the root. tmpfs says
 * its own limits; /proc and /dev are not on anything, and say what they
 * are with nothing in them, as Linux's proc and devtmpfs do.
 */
int vfs_statfs(const char *path, struct statfs *s)
{
    struct fs_type *fs = mounted_fs;
    char rbuf[PATH_MAX];
    struct stat st;
    int r;

    if (!mounted_fs) {
        return -ENODEV;
    }
    if (path) {
        r = vfs_stat(path, &st);        /* it must exist, as on Linux */
        if (r < 0) {
            return r;
        }
        fs = vfs_route(&path, rbuf);
        if ((fs != mounted_fs && !fs->statfs) || proc_owns(path) ||
            dev_is_dir(path) || resolve_dev(path)) {
            memset(s, 0, sizeof(*s));
            s->f_type = proc_owns(path) ? PROC_SUPER_MAGIC : TMPFS_MAGIC;
            s->f_bsize = s->f_frsize = 4096;
            s->f_namelen = 255;
            return 0;
        }
    }
    if (!fs->statfs) {
        return -ENOSYS;
    }
    fs_lock();
    r = fs->statfs(path, s);
    fs_unlock();
    return r;
}

/* ---------------------------------------------------------------- */
/* Mounting more volumes                                             */
/* ---------------------------------------------------------------- */

/*
 * mount(2) and umount2(2). The volumes are the root filesystem's to
 * keep (see mount_on in vfs.h): what is decided here is who may, and
 * where. Only root mounts, as on Linux; the mount point has to be a
 * directory ON THE DISK -- /tmp, /proc and /dev are routed by name
 * before the disk ever sees the path, so a volume mounted on one of
 * them could never be reached.
 */
static int mount_point_ok(const char *dir)
{
    char rbuf[PATH_MAX];
    const char *p = dir;

    if (vfs_route(&p, rbuf) != mounted_fs || proc_owns(p) ||
        dev_is_dir(p) || resolve_dev(p)) {
        return -EINVAL;
    }
    return 0;
}

int vfs_mount_on(const char *source, const char *dir, const char *type,
                 u32 flags)
{
    struct blockdev *b;
    int r;

    if (!mounted_fs) {
        return -ENODEV;
    }
    if (current && current->euid != 0) {
        return -EPERM;
    }
    if (type && strcmp(type, "ext2") != 0 && strcmp(type, "ext3") != 0 &&
        strcmp(type, "auto") != 0) {
        return -ENODEV;         /* Linux's answer for an unknown type */
    }
    if (strncmp(source, DEV_PREFIX, DEV_PREFIX_LEN) == 0) {
        source += DEV_PREFIX_LEN;
    }
    b = dev_find_block(source);
    if (!b) {
        return -ENOENT;         /* ENOTBLK for a non-device, on Linux */
    }
    r = mount_point_ok(dir);
    if (r < 0) {
        return r;
    }
    if (!mounted_fs->mount_on) {
        return -EINVAL;
    }
    fs_lock();
    r = mounted_fs->mount_on(dir, b, flags);
    fs_unlock();
    return r;
}

int vfs_umount_on(const char *dir, u32 flags)
{
    int r;

    if (!mounted_fs) {
        return -ENODEV;
    }
    if (current && current->euid != 0) {
        return -EPERM;
    }
    r = mount_point_ok(dir);
    if (r < 0) {
        return r;
    }
    if (!mounted_fs->umount_on) {
        return -EINVAL;
    }
    fs_lock();
    r = mounted_fs->umount_on(dir, flags);
    fs_unlock();
    if (r == 0) {
        /* Pages cached from it are keyed by inode number, which the
         * next volume mounted there will reuse. */
        textcache_forget_all();
    }
    return r;
}

int vfs_mount_list(int i, struct mount_entry *m)
{
    int r;

    if (!mounted_fs || !mounted_fs->mount_list) {
        if (i == 0 && mounted_fs) {
            memset(m, 0, sizeof(*m));
            strncpy(m->source, mounted_dev->name, sizeof(m->source) - 1);
            strcpy(m->dir, "/");
            return 0;
        }
        return -ENOENT;
    }
    fs_lock();
    r = mounted_fs->mount_list(i, m);
    fs_unlock();
    return r;
}

/*
 * Does any task have its working directory or its root on an inode
 * whose handle is in [lo, hi]? What umount asks before letting a volume
 * go. A working directory in tmpfs or /proc keeps the disk's old number
 * (see vfs_chdir) and is not on the disk at all, so it is not counted.
 */
int vfs_handles_in(u32 lo, u32 hi)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        struct task *t = task_slot(i);

        if (!t || t->state == TASK_UNUSED) {
            continue;
        }
        if (t->root_ino >= lo && t->root_ino <= hi) {
            return 1;
        }
        if (t->cwd_ino >= lo && t->cwd_ino <= hi &&
            !tmp_path(t->cwd_path) && !proc_owns(t->cwd_path)) {
            return 1;
        }
    }
    return 0;
}

/* A volume's name: the one `path` is on, or the root's. See
 * FSCTL_LABEL in uapi.h. */
int vfs_label(const char *path, struct fslabel *l)
{
    if (!mounted_fs) {
        return -ENODEV;
    }
    if (!mounted_fs->label) {
        return -ENOSYS;
    }
    if (path && vfs_tmp_owns(path)) {
        return -EINVAL;         /* not a volume: no name */
    }
    {
        int r;

        fs_lock();
        r = mounted_fs->label(path, l);
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
    if (vfs_tmp_owns(path)) {
        return -EINVAL;         /* a root has to be on the disk */
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

/* ---------------------------------------------------------------- */
/* inotify                                                           */
/* ---------------------------------------------------------------- */

/*
 * The calls that change something, each wrapped so that a change that
 * SUCCEEDED is reported to inotify (events.c). Nothing in the bodies
 * above knows about it, and every hook costs one test of
 * inotify_watching while nothing is watched. Removal and renaming look
 * at their victim first, because afterwards it is not there to look at.
 */
int fd_open_mode(const char *path, int flags, u32 mode)
{
    struct stat st;
    int existed = 1, fd;

    if (inotify_watching && (flags & O_CREAT)) {
        existed = vfs_lstat(path, &st) == 0;
    }
    fd = fd_open_mode_raw(path, flags, mode);
    if (fd >= 0 && inotify_watching) {
        if (!existed) {
            inotify_path(path, IN_CREATE, 0);
        } else if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY) {
            inotify_file(fd_get(fd), IN_MODIFY);
        }
        inotify_file(fd_get(fd), IN_OPEN);
    }
    return fd;
}

s32 fd_read(int fd, void *buf, u32 len)
{
    s32 n = fd_read_raw(fd, buf, len);

    if (n > 0 && inotify_watching) {
        inotify_file(fd_get(fd), IN_ACCESS);
    }
    return n;
}

s32 fd_write(int fd, const void *buf, u32 len)
{
    s32 n = fd_write_raw(fd, buf, len);

    if (n > 0 && inotify_watching) {
        inotify_file(fd_get(fd), IN_MODIFY);
    }
    return n;
}

s32 fd_pwrite(int fd, const void *buf, u32 len, u32 off)
{
    s32 n = fd_pwrite_raw(fd, buf, len, off);

    if (n > 0 && inotify_watching) {
        inotify_file(fd_get(fd), IN_MODIFY);
    }
    return n;
}

int vfs_mkdir_mode(const char *path, u32 mode)
{
    int r = vfs_mkdir_mode_raw(path, mode);

    if (r == 0) {
        inotify_path(path, IN_CREATE, 0);
    }
    return r;
}

int vfs_rmdir(const char *path)
{
    struct inotify_victim v;
    int r;

    inotify_look(path, &v);
    r = vfs_rmdir_raw(path);
    if (r == 0) {
        inotify_removed(path, &v);
    }
    return r;
}

int vfs_unlink(const char *path)
{
    struct inotify_victim v;
    int r;

    inotify_look(path, &v);
    r = vfs_unlink_raw(path);
    if (r == 0) {
        inotify_removed(path, &v);
    }
    return r;
}

int vfs_link(const char *from, const char *to)
{
    int r = vfs_link_raw(from, to);

    if (r == 0) {
        inotify_path(to, IN_CREATE, IN_ATTRIB);
    }
    return r;
}

/*
 * memfd_create: a file with no name, in memory. It is a tmpfs file made
 * under /dev/shm and unlinked at once -- which is all a memfd is on
 * Linux too, a shmem file nobody can open by name -- so it reads,
 * writes, truncates and maps MAP_SHARED like any tmpfs file, and goes
 * when its last descriptor does. /proc names it "/memfd:NAME (deleted)",
 * as Linux does. Sealing is not implemented: MFD_ALLOW_SEALING is
 * accepted, and F_ADD_SEALS is EINVAL, which is what a program sees on
 * a Linux file that cannot be sealed.
 */
int vfs_memfd(const char *name, u32 flags)
{
    static u32 seq;
    char path[40], label[PATH_MAX];
    int fd, tries;

    if (flags & ~(u32)(MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
        return -EINVAL;
    }
    if (strlen(name) > 249) {
        return -EINVAL;                 /* Linux's limit */
    }
    for (tries = 0; tries < 1000; tries++) {
        u32 v = ++seq, n = 0, i;
        char num[12];

        do {
            num[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        strcpy(path, "/dev/shm/.memfd-");
        i = (u32)strlen(path);
        while (n) {
            path[i++] = num[--n];
        }
        path[i] = '\0';
        fd = fd_open_mode(path, O_CREAT | O_EXCL | O_RDWR |
                          ((flags & MFD_CLOEXEC) ? O_CLOEXEC : 0), 0600);
        if (fd != -EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        return fd;
    }
    vfs_unlink(path);
    strcpy(label, "/memfd:");
    strcpy(label + 7, name);
    strcpy(label + 7 + strlen(name), " (deleted)");
    vfs_file_set_name(fd_get(fd), label);
    return fd;
}

int vfs_mknod(const char *path, u32 mode)
{
    int r = vfs_mknod_raw(path, mode);

    if (r == 0 && S_ISFIFO(mode)) {
        inotify_path(path, IN_CREATE, 0);
    }
    return r;
}

int vfs_symlink(const char *target, const char *linkpath)
{
    int r = vfs_symlink_raw(target, linkpath);

    if (r == 0) {
        inotify_path(linkpath, IN_CREATE, 0);
    }
    return r;
}

int vfs_rename(const char *from, const char *to)
{
    struct inotify_victim v, replaced;
    int r;

    inotify_look(from, &v);
    inotify_look(to, &replaced);
    r = vfs_rename_raw(from, to);
    if (r == 0) {
        inotify_moved(from, to, &v, &replaced);
    }
    return r;
}

int vfs_setattr(const char *path, u32 mask, u32 mode, u32 uid, u32 gid)
{
    int r = vfs_setattr_raw(path, mask, mode, uid, gid);

    if (r == 0) {
        inotify_path(path, IN_ATTRIB, IN_ATTRIB);
    }
    return r;
}

int vfs_fsetattr(int fd, u32 mask, u32 mode, u32 uid, u32 gid)
{
    int r = vfs_fsetattr_raw(fd, mask, mode, uid, gid);

    if (r == 0 && inotify_watching) {
        inotify_file(fd_get(fd), IN_ATTRIB);
    }
    return r;
}

int vfs_utime(const char *path, u32 mtime, u32 atime)
{
    int r = vfs_utime_raw(path, mtime, atime);

    if (r == 0) {
        inotify_path(path, IN_ATTRIB, IN_ATTRIB);
    }
    return r;
}

int vfs_futime(int fd, u32 mtime, u32 atime)
{
    int r = vfs_futime_raw(fd, mtime, atime);

    if (r == 0 && inotify_watching) {
        inotify_file(fd_get(fd), IN_ATTRIB);
    }
    return r;
}

int vfs_ftruncate(int fd, u32 len)
{
    int r = vfs_ftruncate_raw(fd, len);

    if (r == 0 && inotify_watching) {
        inotify_file(fd_get(fd), IN_MODIFY);
    }
    return r;
}
