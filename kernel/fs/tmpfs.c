/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tmpfs.c - a filesystem in memory, at /tmp and /dev/shm.
 *
 * Files here are pages from the page allocator and nothing else: they
 * cost no disk writes, and they are gone at the next boot instead of
 * surviving a crash as litter in /tmp. /dev/shm is where shm_open(3)
 * puts POSIX shared memory -- a file here, mapped MAP_SHARED, which is
 * the page cache's business (textcache.c) exactly as it is for a file
 * on the disk.
 *
 * ONE filesystem with two tops: this file's namespace has "/tmp" and
 * "/shm" under its own root, and vfs.c sends "/tmp/..." here as
 * "/tmp/..." and "/dev/shm/..." as "/shm/...". It is a `struct fs_type`
 * like ext2's, so everything vfs.c does for a file -- permissions,
 * the text cache, shared mappings, descriptors -- is done for these the
 * same way, with this answering underneath.
 *
 * What it is: regular files, directories, symbolic links and hard
 * links, with modes, owners and modification times; the sticky bit on
 * /tmp is enforced (only a file's owner, the directory's owner or root
 * may remove or rename it). A file's data is a two-level table of pages
 * -- 4 GB, which is more than the machine has -- and pages never
 * written are holes that read as zero.
 *
 * What it is not: a place for a symbolic link that leads OUT of it. The
 * paths this is handed are its own, and a link to /etc/passwd from /tmp
 * is refused with EXDEV rather than followed onto the wrong
 * filesystem. Nor is statfs asked of it; df reports the disk.
 *
 * Limits: TMP_NODES files and directories, TMP_DENTS names, and half
 * the machine's memory in pages, beyond which ENOSPC.
 */
#include "kernel.h"
#include "vfs.h"
#include "pmm.h"
#include "timer.h"
#include "errno.h"
#include "string.h"

#define TMP_NODES       1024
#define TMP_DENTS       1024
#define TMP_INO_BASE    0x70000000UL    /* no disk inode is this high */
#define PTRS            (PAGE_SIZE / 4) /* 1024 per table page */

struct tnode {
    u32 used;
    u32 ino;                    /* never reused: see tmp_alloc */
    u32 mode, uid, gid, nlink, size;
    time_t mtime;
    u32 opens;                  /* open files on it */
    u32 top;                    /* page of PTRS index pages, or 0 */
};

struct tdent {
    u16 dir;                    /* node index of the directory; 0 free */
    u16 node;
    char name[NAME_MAX + 1];
};

static struct tnode nodes[TMP_NODES];
static struct tdent dents[TMP_DENTS];
static u32 next_ino = 2;
static u32 pages_used, pages_max;

#define ROOT    1

static time_t now(void)
{
    struct timeval tv;

    clock_get(&tv);
    return tv.tv_sec;
}

/* --- pages ----------------------------------------------------------- */

/* The page holding byte `off` of node n: 0 for a hole, unless `make`. */
static u32 page_of(struct tnode *n, u32 off, int make)
{
    u32 p = off / PAGE_SIZE, *top, *idx;

    if (!n->top) {
        if (!make || !(n->top = pmm_alloc())) {
            return 0;
        }
        pages_used++;
    }
    top = (u32 *)n->top;
    if (!top[p / PTRS]) {
        if (!make || !(top[p / PTRS] = pmm_alloc())) {
            return 0;
        }
        pages_used++;
    }
    idx = (u32 *)top[p / PTRS];
    if (!idx[p % PTRS] && make) {
        if (pages_used >= pages_max || !(idx[p % PTRS] = pmm_alloc())) {
            return 0;
        }
        pages_used++;
    }
    return idx[p % PTRS];
}

/* Give back every page from byte `from` on; zero the rest of the page
 * that `from` falls in, so a file that grows again reads zero there. */
static void cut(struct tnode *n, u32 from)
{
    u32 *top = (u32 *)n->top, i, j;

    if (!top) {
        return;
    }
    if (from % PAGE_SIZE) {
        u32 pa = page_of(n, from, 0);

        if (pa) {
            memset((u8 *)pa + from % PAGE_SIZE, 0, PAGE_SIZE - from % PAGE_SIZE);
        }
        from = PAGE_ALIGN_UP(from);
    }
    for (i = 0; i < PTRS; i++) {
        u32 *idx = (u32 *)top[i];
        int any = 0;

        if (!idx) {
            continue;
        }
        for (j = 0; j < PTRS; j++) {
            if (idx[j] && (i * PTRS + j) * PAGE_SIZE >= from) {
                pmm_free(idx[j]);
                idx[j] = 0;
                pages_used--;
            }
            any |= idx[j] != 0;
        }
        if (!any) {
            pmm_free((u32)idx);
            top[i] = 0;
            pages_used--;
        }
    }
    if (from == 0) {
        pmm_free(n->top);
        n->top = 0;
        pages_used--;
    }
}

/* --- nodes and names --------------------------------------------------- */

static void init_once(void)
{
    struct tnode *r;
    static const char *tops[2] = { "tmp", "shm" };
    int i;

    if (nodes[ROOT].used) {
        return;
    }
    pages_max = pmm_total() / 2;
    r = &nodes[ROOT];
    r->used = 1;
    r->ino = TMP_INO_BASE + 1;
    r->mode = S_IFDIR | 0755;
    r->nlink = 4;
    r->mtime = now();
    for (i = 0; i < 2; i++) {
        struct tnode *d = &nodes[2 + i];

        d->used = 1;
        d->ino = TMP_INO_BASE + next_ino++;
        d->mode = S_IFDIR | 01777;      /* anyone may make files; sticky */
        d->nlink = 2;
        d->mtime = r->mtime;
        dents[i].dir = ROOT;
        dents[i].node = (u16)(2 + i);
        strcpy(dents[i].name, tops[i]);
    }
}

static int node_alloc(u32 mode)
{
    u32 uid, gid;
    int i;

    for (i = ROOT + 1; i < TMP_NODES; i++) {
        if (!nodes[i].used) {
            struct tnode *n = &nodes[i];

            memset(n, 0, sizeof(*n));
            n->used = 1;
            /* A new number every time, never an index: a number a freed
             * file had would find its pages still in the text cache. */
            n->ino = TMP_INO_BASE + next_ino++;
            vfs_cred(&uid, &gid);
            n->mode = mode;
            n->uid = uid;
            n->gid = gid;
            n->mtime = now();
            return i;
        }
    }
    return -ENOSPC;
}

/* A node nothing names and nothing has open is gone. */
static void node_maybe_free(int i)
{
    struct tnode *n = &nodes[i];

    if (n->used && n->nlink == 0 && n->opens == 0) {
        cut(n, 0);
        n->used = 0;
    }
}

static int dent_find(int dir, const char *name, u32 len)
{
    int i;

    for (i = 0; i < TMP_DENTS; i++) {
        if (dents[i].dir == dir && strlen(dents[i].name) == len &&
            strncmp(dents[i].name, name, len) == 0) {
            return i;
        }
    }
    return -1;
}

static int dent_add(int dir, const char *name, u32 len, int node)
{
    int i;

    if (len > NAME_MAX) {
        return -ENAMETOOLONG;
    }
    for (i = 0; i < TMP_DENTS; i++) {
        if (!dents[i].dir) {
            dents[i].dir = (u16)dir;
            dents[i].node = (u16)node;
            memcpy(dents[i].name, name, len);
            dents[i].name[len] = '\0';
            nodes[dir].mtime = now();
            return i;
        }
    }
    return -ENOSPC;
}

/* The directory a directory is in: the name that points at it. */
static int parent_of(int dir)
{
    int i;

    if (dir == ROOT) {
        return ROOT;
    }
    for (i = 0; i < TMP_DENTS; i++) {
        if (dents[i].dir && dents[i].node == dir) {
            return dents[i].dir;
        }
    }
    return ROOT;
}

/* --- walking a path ---------------------------------------------------- */

#define WALK_HOPS   8

static int walk_from(int dir, const char *path, int follow_last, int *out,
                     int *out_dir, const char **last, u32 *lastlen, int hops);

/*
 * Follow symlink node `n`, found in directory `dir`, the rest of the
 * path being done by the caller. A target inside this filesystem is
 * walked; one outside it is EXDEV, as the head of the file says.
 */
static int follow(int dir, int n, int *out, int hops)
{
    char target[NAME_MAX + 1];
    u32 len = nodes[n].size, pa = page_of(&nodes[n], 0, 0);
    const char *t;
    int start;

    if (hops >= WALK_HOPS) {
        return -ELOOP;
    }
    if (len > NAME_MAX || !pa) {
        return -ENOENT;
    }
    memcpy(target, (void *)pa, len);
    target[len] = '\0';
    t = target;
    start = dir;
    if (t[0] == '/') {
        if (strncmp(t, "/tmp", 4) == 0 && (t[4] == '/' || !t[4])) {
            start = ROOT;
        } else if (strncmp(t, "/dev/shm", 8) == 0 && (t[8] == '/' || !t[8])) {
            /* "/dev/shm/x" is "/shm/x" here: drop the "/dev". */
            memmove(target, target + 4, strlen(target + 4) + 1);
            start = ROOT;
        } else {
            return -EXDEV;
        }
    }
    return walk_from(start, t, 1, out, 0, 0, 0, hops + 1);
}

static int walk_from(int dir, const char *path, int follow_last, int *out,
                     int *out_dir, const char **last, u32 *lastlen, int hops)
{
    int cur = dir;

    while (*path == '/') {
        path++;
        cur = ROOT;
    }
    for (;;) {
        const char *c = path;
        u32 len = 0;
        int d, n, is_last;

        while (c[len] && c[len] != '/') {
            len++;
        }
        path = c + len;
        while (*path == '/') {
            path++;
        }
        is_last = (*path == '\0');
        if (len == 0) {
            *out = cur;
            return 0;
        }
        if (!S_ISDIR(nodes[cur].mode)) {
            return -ENOTDIR;
        }
        if (is_last) {
            if (out_dir) {
                *out_dir = cur;
            }
            if (last) {
                *last = c;
                *lastlen = len;
            }
        }
        if (len == 1 && c[0] == '.') {
            n = cur;
        } else if (len == 2 && c[0] == '.' && c[1] == '.') {
            n = parent_of(cur);
        } else {
            d = dent_find(cur, c, len);
            if (d < 0) {
                return -ENOENT;
            }
            n = dents[d].node;
            if (S_ISLNK(nodes[n].mode) && (!is_last || follow_last)) {
                int r = follow(cur, n, &n, hops);

                if (r < 0) {
                    return r;
                }
            }
        }
        if (is_last) {
            *out = n;
            return 0;
        }
        cur = n;
    }
}

/* vfs.c hands over the paths as the system names them: "/dev/shm/x"
 * is "/shm/x" here. */
static const char *here(const char *path)
{
    if (strncmp(path, "/dev/shm", 8) == 0 && (path[8] == '/' || !path[8])) {
        return path + 4;
    }
    return path;
}

static int walk(const char *path, int follow_last, int *out)
{
    init_once();
    return walk_from(ROOT, here(path), follow_last, out, 0, 0, 0, 0);
}

/* The directory a new name goes in, and the name. -EEXIST if it is
 * there, unless `may_exist`, when *existing says what it is. */
static int walk_parent(const char *path, int *dir, const char **name,
                       u32 *len, int *existing)
{
    int n, r;

    init_once();
    *existing = -1;
    *name = 0;
    *len = 0;
    r = walk_from(ROOT, here(path), 0, &n, dir, name, len, 0);
    if (r == 0) {
        *existing = n;
        return *name ? 0 : -EBUSY;      /* the root itself has no name */
    }
    /* Only the LAST name missing, its directory found: a new name. It
     * is set only once the walk got that far. */
    if (r == -ENOENT && *name && S_ISDIR(nodes[*dir].mode)) {
        return 0;
    }
    return r;
}

/* The sticky bit: in such a directory only the file's owner, the
 * directory's owner or root may take a name away. */
static int sticky_ok(int dir, int node)
{
    u32 uid, gid;

    vfs_cred(&uid, &gid);
    if (!(nodes[dir].mode & S_ISVTX) || uid == 0) {
        return 0;
    }
    return (uid == nodes[node].uid || uid == nodes[dir].uid) ? 0 : -EPERM;
}

/* --- open files -------------------------------------------------------- */

static s32 tf_read(struct file *f, void *buf, u32 len)
{
    struct tnode *n = &nodes[(u32)f->priv];
    u32 done = 0;

    if (f->pos >= n->size) {
        return 0;
    }
    if (len > n->size - f->pos) {
        len = n->size - f->pos;
    }
    while (done < len) {
        u32 off = f->pos % PAGE_SIZE, k = PAGE_SIZE - off;
        u32 pa = page_of(n, f->pos, 0);

        if (k > len - done) {
            k = len - done;
        }
        if (pa) {
            memcpy((u8 *)buf + done, (u8 *)pa + off, k);
        } else {
            memset((u8 *)buf + done, 0, k);     /* a hole */
        }
        done += k;
        f->pos += k;
    }
    return (s32)done;
}

static s32 tf_write(struct file *f, const void *buf, u32 len)
{
    struct tnode *n = &nodes[(u32)f->priv];
    u32 done = 0;

    if (f->flags & O_APPEND) {
        f->pos = n->size;
    }
    if (f->pos + len < f->pos) {
        return -EFBIG;
    }
    while (done < len) {
        u32 off = f->pos % PAGE_SIZE, k = PAGE_SIZE - off;
        u32 pa = page_of(n, f->pos, 1);

        if (!pa) {
            break;
        }
        if (k > len - done) {
            k = len - done;
        }
        memcpy((u8 *)pa + off, (const u8 *)buf + done, k);
        done += k;
        f->pos += k;
        if (f->pos > n->size) {
            n->size = f->pos;
        }
    }
    if (done) {
        n->mtime = now();
        return (s32)done;
    }
    return len ? -ENOSPC : 0;
}

static s32 tf_lseek(struct file *f, s32 offset, int whence)
{
    struct tnode *n = &nodes[(u32)f->priv];
    s32 base = whence == SEEK_SET ? 0 :
               whence == SEEK_CUR ? (s32)f->pos :
               whence == SEEK_END ? (s32)n->size : -1;

    if (base < 0 || base + offset < 0) {
        return -EINVAL;
    }
    f->pos = (u32)(base + offset);
    return (s32)f->pos;
}

static int tf_close(struct file *f)
{
    u32 i = (u32)f->priv;

    nodes[i].opens--;
    node_maybe_free((int)i);
    return 0;
}

static void fill_stat(int i, struct stat *st)
{
    struct tnode *n = &nodes[i];

    memset(st, 0, sizeof(*st));
    st->st_mode = n->mode;
    st->st_size = S_ISDIR(n->mode) ? PAGE_SIZE : n->size;
    st->st_mtime = n->mtime;
    st->st_ino = n->ino;
    st->st_uid = n->uid;
    st->st_gid = n->gid;
    st->st_nlink = n->nlink;
    st->st_blocks = n->size / 512;
}

static int tf_fstat(struct file *f, struct stat *st)
{
    fill_stat((int)(u32)f->priv, st);
    return 0;
}

static int tf_truncate(struct file *f, u32 len)
{
    struct tnode *n = &nodes[(u32)f->priv];

    if (len < n->size) {
        cut(n, len);
    }
    n->size = len;
    n->mtime = now();
    return 0;
}

static const struct file_ops tmp_file_ops = {
    tf_read,
    tf_write,
    tf_lseek,
    0,
    tf_close,
    tf_fstat,
    0,
    tf_truncate,
    0,                          /* mmap: through the text cache */
};

/* --- the operations vfs.c calls ---------------------------------------- */

static int t_open(const char *path, int flags, struct file *f)
{
    int n, dir, existing, r;
    const char *name;
    u32 len;

    r = walk_parent(path, &dir, &name, &len, &existing);
    if (r < 0) {
        return r;
    }
    if (existing >= 0) {
        n = existing;
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            return -EEXIST;
        }
        if (S_ISLNK(nodes[n].mode)) {
            if (flags & O_NOFOLLOW) {
                return -ELOOP;
            }
            r = walk(path, 1, &n);
            if (r < 0) {
                return r;
            }
        }
        if (S_ISDIR(nodes[n].mode)) {
            return -EISDIR;
        }
        if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY) {
            cut(&nodes[n], 0);
            nodes[n].size = 0;
            nodes[n].mtime = now();
        }
    } else {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        n = node_alloc(S_IFREG | 0644);
        if (n < 0) {
            return n;
        }
        r = dent_add(dir, name, len, n);
        if (r < 0) {
            nodes[n].used = 0;
            return r;
        }
        nodes[n].nlink = 1;
    }
    nodes[n].opens++;
    f->ops = &tmp_file_ops;
    f->priv = (void *)(u32)n;
    f->pos = (flags & O_APPEND) ? nodes[n].size : 0;
    return 0;
}

static int t_stat(const char *path, struct stat *st)
{
    int n, r = walk(path, 1, &n);

    if (r < 0) {
        return r;
    }
    fill_stat(n, st);
    return 0;
}

static int t_lstat(const char *path, struct stat *st)
{
    int n, r = walk(path, 0, &n);

    if (r < 0) {
        return r;
    }
    fill_stat(n, st);
    return 0;
}

static int t_unlink_common(const char *path, int want_dir)
{
    int dir, existing, d, r;
    const char *name;
    u32 len;

    r = walk_parent(path, &dir, &name, &len, &existing);
    if (r < 0) {
        return r;
    }
    if (existing < 0) {
        return -ENOENT;
    }
    if (dir == ROOT) {
        return -EBUSY;          /* /tmp and /dev/shm themselves */
    }
    if (want_dir && !S_ISDIR(nodes[existing].mode)) {
        return -ENOTDIR;
    }
    if (!want_dir && S_ISDIR(nodes[existing].mode)) {
        return -EISDIR;
    }
    r = sticky_ok(dir, existing);
    if (r < 0) {
        return r;
    }
    if (want_dir) {
        for (d = 0; d < TMP_DENTS; d++) {
            if (dents[d].dir == existing) {
                return -ENOTEMPTY;
            }
        }
    }
    d = dent_find(dir, name, len);
    dents[d].dir = 0;
    nodes[dir].mtime = now();
    if (want_dir) {
        nodes[existing].nlink = 0;
        nodes[dir].nlink--;
    } else {
        nodes[existing].nlink--;
    }
    node_maybe_free(existing);
    return 0;
}

static int t_unlink(const char *path)
{
    return t_unlink_common(path, 0);
}

static int t_rmdir(const char *path)
{
    return t_unlink_common(path, 1);
}

static int t_mkdir(const char *path)
{
    int dir, existing, n, r;
    const char *name;
    u32 len;

    r = walk_parent(path, &dir, &name, &len, &existing);
    if (r < 0) {
        return r;
    }
    if (existing >= 0) {
        return -EEXIST;
    }
    n = node_alloc(S_IFDIR | 0755);
    if (n < 0) {
        return n;
    }
    r = dent_add(dir, name, len, n);
    if (r < 0) {
        nodes[n].used = 0;
        return r;
    }
    nodes[n].nlink = 2;
    nodes[dir].nlink++;
    return 0;
}

static int t_symlink(const char *target, const char *linkpath)
{
    int dir, existing, n, r;
    const char *name;
    u32 len, tl = (u32)strlen(target), pa;

    if (tl > NAME_MAX) {
        return -ENAMETOOLONG;
    }
    r = walk_parent(linkpath, &dir, &name, &len, &existing);
    if (r < 0) {
        return r;
    }
    if (existing >= 0) {
        return -EEXIST;
    }
    n = node_alloc(S_IFLNK | 0777);
    if (n < 0) {
        return n;
    }
    pa = page_of(&nodes[n], 0, 1);
    if (!pa) {
        nodes[n].used = 0;
        return -ENOSPC;
    }
    memcpy((void *)pa, target, tl);
    nodes[n].size = tl;
    r = dent_add(dir, name, len, n);
    if (r < 0) {
        cut(&nodes[n], 0);
        nodes[n].used = 0;
        return r;
    }
    nodes[n].nlink = 1;
    return 0;
}

static int t_readlink(const char *path, char *out, u32 size)
{
    int n, r = walk(path, 0, &n);
    u32 pa;

    if (r < 0) {
        return r;
    }
    if (!S_ISLNK(nodes[n].mode)) {
        return -EINVAL;
    }
    if (nodes[n].size >= size) {
        return -ENAMETOOLONG;
    }
    pa = page_of(&nodes[n], 0, 0);
    memcpy(out, (void *)pa, nodes[n].size);
    out[nodes[n].size] = '\0';
    return 0;
}

static int t_link(const char *from, const char *to)
{
    int n, dir, existing, r;
    const char *name;
    u32 len;

    r = walk(from, 0, &n);
    if (r < 0) {
        return r;
    }
    if (S_ISDIR(nodes[n].mode)) {
        return -EPERM;
    }
    r = walk_parent(to, &dir, &name, &len, &existing);
    if (r < 0) {
        return r;
    }
    if (existing >= 0) {
        return -EEXIST;
    }
    r = dent_add(dir, name, len, n);
    if (r < 0) {
        return r;
    }
    nodes[n].nlink++;
    return 0;
}

/* Is `a` the directory `d` or somewhere under it? */
static int is_under(int a, int d)
{
    int hops = 0;

    while (hops++ < TMP_NODES) {
        if (a == d) {
            return 1;
        }
        if (a == ROOT) {
            return 0;
        }
        a = parent_of(a);
    }
    return 0;
}

static int t_rename(const char *from, const char *to)
{
    int fdir, tdir, fnode, tnode_, r, df, dt;
    const char *fname, *tname;
    u32 flen, tlen;

    r = walk_parent(from, &fdir, &fname, &flen, &fnode);
    if (r < 0) {
        return r;
    }
    if (fnode < 0) {
        return -ENOENT;
    }
    if (fdir == ROOT) {
        return -EBUSY;
    }
    r = walk_parent(to, &tdir, &tname, &tlen, &tnode_);
    if (r < 0) {
        return r;
    }
    if (tdir == ROOT) {
        return -EBUSY;
    }
    if (tnode_ == fnode) {
        return 0;               /* two names for one file: nothing to do */
    }
    if (S_ISDIR(nodes[fnode].mode) && is_under(tdir, fnode)) {
        return -EINVAL;         /* into itself */
    }
    r = sticky_ok(fdir, fnode);
    if (r < 0) {
        return r;
    }
    if (tnode_ >= 0) {
        if (S_ISDIR(nodes[tnode_].mode) != S_ISDIR(nodes[fnode].mode)) {
            return S_ISDIR(nodes[tnode_].mode) ? -EISDIR : -ENOTDIR;
        }
        r = sticky_ok(tdir, tnode_);
        if (r < 0) {
            return r;
        }
        if (S_ISDIR(nodes[tnode_].mode)) {
            for (dt = 0; dt < TMP_DENTS; dt++) {
                if (dents[dt].dir == tnode_) {
                    return -ENOTEMPTY;
                }
            }
            nodes[tdir].nlink--;
            nodes[tnode_].nlink = 0;
        } else {
            nodes[tnode_].nlink--;
        }
        dt = dent_find(tdir, tname, tlen);
        dents[dt].dir = 0;
        node_maybe_free(tnode_);
    }
    df = dent_find(fdir, fname, flen);
    if (tlen > NAME_MAX) {
        return -ENAMETOOLONG;
    }
    dents[df].dir = (u16)tdir;
    memcpy(dents[df].name, tname, tlen);
    dents[df].name[tlen] = '\0';
    if (S_ISDIR(nodes[fnode].mode) && fdir != tdir) {
        nodes[fdir].nlink--;
        nodes[tdir].nlink++;
    }
    nodes[fdir].mtime = nodes[tdir].mtime = now();
    return 0;
}

/* Directories: a directory's identity is its node index. */
static int t_dir_ino(const char *path, u32 *ino)
{
    int n, r = walk(path, 1, &n);

    if (r < 0) {
        return r;
    }
    if (!S_ISDIR(nodes[n].mode)) {
        return -ENOTDIR;
    }
    *ino = (u32)n;
    return 0;
}

static int t_readdir_in(u32 dir, int index, struct dirent *out)
{
    int i, k = 0;

    if (dir >= TMP_NODES || !nodes[dir].used || !S_ISDIR(nodes[dir].mode)) {
        return -ENOTDIR;
    }
    memset(out, 0, sizeof(*out));
    if (index < 2) {
        int n = index ? parent_of((int)dir) : (int)dir;

        strcpy(out->d_name, index ? ".." : ".");
        out->d_mode = nodes[n].mode;
        out->d_ino = nodes[n].ino;
        out->d_mtime = nodes[n].mtime;
        return 0;
    }
    for (i = 0; i < TMP_DENTS; i++) {
        if (dents[i].dir == dir && k++ == index - 2) {
            struct tnode *n = &nodes[dents[i].node];

            strcpy(out->d_name, dents[i].name);
            out->d_mode = n->mode;
            out->d_size = n->size;
            out->d_mtime = n->mtime;
            out->d_ino = n->ino;
            return 0;
        }
    }
    return -ENOENT;
}

/* The path of directory `dir` in this filesystem's own namespace --
 * "/tmp/a" or "/shm/b" -- which vfs.c turns back into /dev/shm. */
static int t_dir_path(u32 dir, char *out, u32 size)
{
    char tmp[PATH_MAX];
    u32 n = 0;
    int d = (int)dir, hops = 0;

    tmp[0] = '\0';
    while (d != ROOT && hops++ < TMP_NODES) {
        int i;
        u32 l;

        for (i = 0; i < TMP_DENTS; i++) {
            if (dents[i].dir && dents[i].node == d) {
                break;
            }
        }
        if (i == TMP_DENTS) {
            return -ENOENT;
        }
        l = (u32)strlen(dents[i].name);
        if (n + l + 2 >= sizeof(tmp)) {
            return -ENAMETOOLONG;
        }
        memmove(tmp + l + 1, tmp, n + 1);
        tmp[0] = '/';
        memcpy(tmp + 1, dents[i].name, l);
        n += l + 1;
        d = dents[i].dir;
    }
    if (n == 0) {
        strcpy(tmp, "/");
        n = 1;
    }
    if (n >= size) {
        return -ENAMETOOLONG;
    }
    strcpy(out, tmp);
    return 0;
}

static int t_utime(const char *path, u32 mtime, u32 atime)
{
    int n, r = walk(path, 1, &n);

    (void)atime;
    if (r < 0) {
        return r;
    }
    if (mtime != (u32)-1) {
        nodes[n].mtime = (time_t)mtime;
    }
    return 0;
}

static int t_futime(struct file *f, u32 mtime, u32 atime)
{
    (void)atime;
    if (mtime != (u32)-1) {
        nodes[(u32)f->priv].mtime = (time_t)mtime;
    }
    return 0;
}

static void set_attr(int n, u32 mask, u32 mode, u32 uid, u32 gid)
{
    if (mask & ATTR_MODE) {
        nodes[n].mode = (nodes[n].mode & S_IFMT) | (mode & 07777);
    }
    if (mask & ATTR_UID) {
        nodes[n].uid = uid;
    }
    if (mask & ATTR_GID) {
        nodes[n].gid = gid;
    }
}

static int t_setattr(const char *path, u32 mask, u32 mode, u32 uid, u32 gid)
{
    int n, r = walk(path, 1, &n);

    if (r < 0) {
        return r;
    }
    set_attr(n, mask, mode, uid, gid);
    return 0;
}

static int t_fsetattr(struct file *f, u32 mask, u32 mode, u32 uid, u32 gid)
{
    set_attr((int)(u32)f->priv, mask, mode, uid, gid);
    return 0;
}

static struct fs_type tmpfs_type = {
    .name = "tmpfs",
    .open = t_open,
    .unlink = t_unlink,
    .rename = t_rename,
    .stat = t_stat,
    .mkdir = t_mkdir,
    .rmdir = t_rmdir,
    .readdir_in = t_readdir_in,
    .dir_ino = t_dir_ino,
    .dir_path = t_dir_path,
    .utime = t_utime,
    .futime = t_futime,
    .setattr = t_setattr,
    .fsetattr = t_fsetattr,
    .link = t_link,
    .symlink = t_symlink,
    .readlink = t_readlink,
    .lstat = t_lstat,
};

struct fs_type *tmpfs_fs(void)
{
    init_once();
    return &tmpfs_type;
}

int tmpfs_is_file(const struct file *f)
{
    return f && f->ops == &tmp_file_ops;
}

void tmpfs_usage(u32 *used, u32 *max)
{
    *used = pages_used;
    *max = pages_max;
}
