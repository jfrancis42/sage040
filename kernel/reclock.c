/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * reclock.c - POSIX record locks: fcntl F_GETLK, F_SETLK, F_SETLKW, and
 * Linux's open-file-description locks, F_OFD_*.
 *
 * A lock is a byte range of a file, shared (read) or exclusive (write),
 * held by an OWNER. For the POSIX kind the owner is the PROCESS -- so
 * two descriptors in one process never conflict, and closing ANY
 * descriptor for the file drops every lock the process has on it, which
 * is POSIX's famous wart and exactly what SQLite is written around. For
 * the OFD kind the owner is the open file description, as with flock():
 * a dup shares it, a second open() is a different owner, and it lasts
 * until the description's last close. The two kinds conflict with each
 * other as they do on Linux.
 *
 * A process's own locks never conflict with each other; taking one over
 * a range it already holds REPLACES what was there -- splitting a range
 * where needed, merging neighbours of the same type -- which is how a
 * shared lock is upgraded and how part of one is released.
 *
 * Files are known by flock_key() (the inode number), as flock's are.
 * Advisory, as everywhere: nothing stops a program that does not ask.
 */
#include "reclock.h"
#include "vfs.h"
#include "dev.h"
#include "task.h"
#include "signal.h"
#include "wait.h"
#include "sysint.h"
#include "errno.h"
#include "string.h"

#define RECLOCK_MAX     128
#define TO_EOF          0xffffffffUL

struct reclock {
    int used;
    u32 key;                    /* the file                             */
    int pid;                    /* the owning process (POSIX), or 0     */
    struct file *ofd;           /* the owning description (OFD), or 0   */
    int type;                   /* F_RDLCK or F_WRLCK                   */
    u32 start, end;             /* inclusive; end TO_EOF: to the end    */
};

static struct reclock locks[RECLOCK_MAX];
static struct waitq lock_wait;

/* Who is waiting for whom, for EDEADLK: a process asleep in F_SETLKW
 * and the process holding what it wants. */
#define WAITER_MAX  TASK_MAX

static struct {
    int pid;
    int blocker;
} waiters[WAITER_MAX];

static int same_owner(const struct reclock *l, int pid, struct file *ofd)
{
    return ofd ? l->ofd == ofd : (!l->ofd && l->pid == pid);
}

static int overlaps(const struct reclock *l, u32 s, u32 e)
{
    return l->start <= e && s <= l->end;
}

/* The first lock of someone else's that stands in the way, or null. */
static struct reclock *conflict(u32 key, int pid, struct file *ofd,
                                int type, u32 s, u32 e)
{
    int i;

    for (i = 0; i < RECLOCK_MAX; i++) {
        struct reclock *l = &locks[i];

        if (l->used && l->key == key && !same_owner(l, pid, ofd) &&
            overlaps(l, s, e) && (type == F_WRLCK || l->type == F_WRLCK)) {
            return l;
        }
    }
    return 0;
}

static struct reclock *slot(void)
{
    int i;

    for (i = 0; i < RECLOCK_MAX; i++) {
        if (!locks[i].used) {
            return &locks[i];
        }
    }
    return 0;
}

static int free_slots(void)
{
    int i, n = 0;

    for (i = 0; i < RECLOCK_MAX; i++) {
        n += !locks[i].used;
    }
    return n;
}

/*
 * Make the owner hold `type` over [s, e] -- or nothing there, for
 * F_UNLCK -- whatever it held before. Returns 0 or -ENOLCK, and changes
 * nothing if it fails.
 */
static int set_range(u32 key, int pid, struct file *ofd, int type,
                     u32 s, u32 e)
{
    struct reclock *n;
    int i, splits = 0;

    /* Room first: a range cut out of the middle of one leaves two. */
    for (i = 0; i < RECLOCK_MAX; i++) {
        struct reclock *l = &locks[i];

        if (l->used && l->key == key && same_owner(l, pid, ofd) &&
            l->start < s && l->end > e) {
            splits++;
        }
    }
    if (free_slots() < splits + (type != F_UNLCK)) {
        return -ENOLCK;
    }

    for (i = 0; i < RECLOCK_MAX; i++) {
        struct reclock *l = &locks[i];

        if (!l->used || l->key != key || !same_owner(l, pid, ofd) ||
            !overlaps(l, s, e)) {
            continue;
        }
        if (l->start < s && l->end > e) {
            struct reclock *r = slot();

            *r = *l;
            r->start = e + 1;
            l->end = s - 1;
        } else if (l->start < s) {
            l->end = s - 1;
        } else if (l->end > e) {
            l->start = e + 1;
        } else {
            l->used = 0;
        }
    }
    if (type != F_UNLCK) {
        /* Neighbours of the same type become one range. */
        for (i = 0; i < RECLOCK_MAX; i++) {
            struct reclock *l = &locks[i];

            if (!l->used || l->key != key || !same_owner(l, pid, ofd) ||
                l->type != type) {
                continue;
            }
            if (s > 0 && l->end == s - 1) {
                s = l->start;
                l->used = 0;
            } else if (e != TO_EOF && l->start == e + 1) {
                e = l->end;
                l->used = 0;
            }
        }
        n = slot();
        n->used = 1;
        n->key = key;
        n->pid = ofd ? 0 : pid;
        n->ofd = ofd;
        n->type = type;
        n->start = s;
        n->end = e;
    }
    wake_all(&lock_wait);
    return 0;
}

/* Would waiting on `blocker` close a circle back to `pid`? */
static int would_deadlock(int pid, int blocker)
{
    int depth, i;

    for (depth = 0; depth < WAITER_MAX && blocker; depth++) {
        if (blocker == pid) {
            return 1;
        }
        for (i = 0; i < WAITER_MAX; i++) {
            if (waiters[i].pid == blocker) {
                break;
            }
        }
        blocker = i < WAITER_MAX ? waiters[i].blocker : 0;
    }
    return 0;
}

static void set_waiting(int pid, int blocker)
{
    int i, free_i = -1;

    for (i = 0; i < WAITER_MAX; i++) {
        if (waiters[i].pid == pid) {
            waiters[i].blocker = blocker;
            if (!blocker) {
                waiters[i].pid = 0;
            }
            return;
        }
        if (!waiters[i].pid && free_i < 0) {
            free_i = i;
        }
    }
    if (blocker && free_i >= 0) {
        waiters[free_i].pid = pid;
        waiters[free_i].blocker = blocker;
    }
}

/* ---------------------------------------------------------------- */

/* Both shapes of struct flock, read into one. */
struct lk {
    int type, whence, pid;
    s32 start_hi;
    u32 start;
    s32 len_hi;
    u32 len;
};

static int fetch_lk(u32 u, int wide, struct lk *k)
{
    int err;

    if (wide) {
        struct flock64 f;

        if ((err = fetch(&f, u, sizeof(f))) < 0) {
            return err;
        }
        k->type = f.l_type;
        k->whence = f.l_whence;
        k->start_hi = (s32)((u64)f.l_start >> 32);
        k->start = (u32)f.l_start;
        k->len_hi = (s32)((u64)f.l_len >> 32);
        k->len = (u32)f.l_len;
        k->pid = f.l_pid;
    } else {
        struct flock f;

        if ((err = fetch(&f, u, sizeof(f))) < 0) {
            return err;
        }
        k->type = f.l_type;
        k->whence = f.l_whence;
        k->start_hi = f.l_start < 0 ? -1 : 0;
        k->start = (u32)f.l_start;
        k->len_hi = f.l_len < 0 ? -1 : 0;
        k->len = (u32)f.l_len;
        k->pid = f.l_pid;
    }
    return 0;
}

static int store_lk(u32 u, int wide, int type, u32 s, u32 e, int pid)
{
    u32 len = e == TO_EOF ? 0 : e - s + 1;

    if (wide) {
        struct flock64 f;

        memset(&f, 0, sizeof(f));
        f.l_type = (s16)type;
        f.l_whence = SEEK_SET;
        f.l_start = s;
        f.l_len = len;
        f.l_pid = pid;
        return sys_store(u, &f, sizeof(f));
    } else {
        struct flock f;

        memset(&f, 0, sizeof(f));
        f.l_type = (s16)type;
        f.l_whence = SEEK_SET;
        f.l_start = (s32)s;
        f.l_len = (s32)len;
        f.l_pid = pid;
        return sys_store(u, &f, sizeof(f));
    }
}

/*
 * A struct flock's range as [*s, *e]: from whence, and a negative
 * length reaching BACK from start, as POSIX allows. 32-bit files here,
 * so anything that needs more than 32 bits is EOVERFLOW, except a start
 * or an end past 4 GB, which is simply "to the end".
 */
static int range_of(struct file *f, const struct lk *k, u32 *s, u32 *e)
{
    s32 base_hi = 0;
    u32 base = 0, start;
    s32 start_hi;

    switch (k->whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = f->pos;
        break;
    case SEEK_END: {
        struct stat st;

        if (vfs_file_stat(f, &st) < 0) {
            return -EINVAL;
        }
        base = st.st_size;
        break;
    }
    default:
        return -EINVAL;
    }
    start = base + k->start;
    start_hi = base_hi + k->start_hi + (start < base);
    if (start_hi < 0) {
        return -EINVAL;
    }
    if (start_hi > 0) {
        return -EOVERFLOW;
    }
    if (k->len_hi == 0 && k->len == 0) {
        *s = start;
        *e = TO_EOF;
    } else if (k->len_hi >= 0) {
        u32 end = start + k->len - 1;

        *s = start;
        *e = (k->len_hi > 0 || end < start) ? TO_EOF : end;
    } else {
        /* Negative: the len bytes BEFORE start. */
        u32 back = (u32)-(s32)k->len;

        if (k->len_hi != -1 || back > start) {
            return -EINVAL;
        }
        *s = start - back;
        *e = start - 1;
    }
    return 0;
}

int reclock_fcntl(int fd, int cmd, u32 uarg, int wide)
{
    struct file *f = fd_get(fd);
    struct file *ofd = 0;
    int pid = current->tgid, blocking = 0, getting = 0, acc, err;
    struct lk k;
    u32 key, s, e;

    if (!f) {
        return -EBADF;
    }
    switch (cmd) {
    case F_GETLK64:
        wide = 1;
        /* fall through */
    case F_GETLK:
        wide = 0;               /* struct flock, from fcntl64 too */
        getting = 1;
        break;
    case F_SETLKW64:
        wide = 1;
        /* fall through */
    case F_SETLKW:
        wide = 0;
        blocking = 1;
        break;
    case F_SETLK64:
        wide = 1;
        /* fall through */
    case F_SETLK:
        wide = 0;
        break;
    case F_OFD_GETLK:
        getting = 1;
        ofd = f;
        break;
    case F_OFD_SETLKW:
        blocking = 1;
        ofd = f;
        break;
    case F_OFD_SETLK:
        ofd = f;
        break;
    default:
        return -EINVAL;
    }
    if ((err = fetch_lk(uarg, wide, &k)) < 0) {
        return err;
    }
    if (k.type != F_RDLCK && k.type != F_WRLCK && k.type != F_UNLCK) {
        return -EINVAL;
    }
    if (ofd && k.pid != 0) {
        return -EINVAL;         /* Linux's rule for the OFD calls */
    }
    if ((err = range_of(f, &k, &s, &e)) < 0) {
        return err;
    }
    key = flock_key(f);

    if (getting) {
        struct reclock *c;

        if (k.type == F_UNLCK) {
            return -EINVAL;
        }
        c = conflict(key, pid, ofd, k.type, s, e);
        if (!c) {
            return store_lk(uarg, wide, F_UNLCK, s, e, 0);
        }
        return store_lk(uarg, wide, c->type, c->start, c->end,
                        c->ofd ? -1 : c->pid);
    }

    /* A read lock needs a descriptor open for reading; a write lock,
     * one open for writing. */
    acc = f->flags & O_ACCMODE;
    if ((k.type == F_RDLCK && acc == O_WRONLY) ||
        (k.type == F_WRLCK && acc == O_RDONLY)) {
        return -EBADF;
    }
    for (;;) {
        struct reclock *c = k.type == F_UNLCK ? 0
                            : conflict(key, pid, ofd, k.type, s, e);

        if (!c) {
            break;
        }
        if (!blocking) {
            return -EAGAIN;
        }
        if (!ofd && !c->ofd && would_deadlock(pid, c->pid)) {
            return -EDEADLK;
        }
        if (signal_pending(current)) {
            set_waiting(pid, 0);
            return -EINTR;
        }
        set_waiting(pid, c->ofd ? 0 : c->pid);
        sleep_on(&lock_wait);
    }
    set_waiting(pid, 0);
    return set_range(key, pid, ofd, k.type, s, e);
}

/* Drop every lock matching; wake anyone waiting if there were any. */
static void drop(int by_key, u32 key, int pid, struct file *ofd)
{
    int i, any = 0;

    for (i = 0; i < RECLOCK_MAX; i++) {
        struct reclock *l = &locks[i];

        if (l->used && (!by_key || l->key == key) &&
            (ofd ? l->ofd == ofd : (!l->ofd && l->pid == pid))) {
            l->used = 0;
            any = 1;
        }
    }
    if (any) {
        wake_all(&lock_wait);
    }
}

void reclock_closed(struct file *f, int pid)
{
    int i;

    for (i = 0; i < RECLOCK_MAX; i++) {
        if (locks[i].used && !locks[i].ofd && locks[i].pid == pid) {
            drop(1, flock_key(f), pid, 0);
            return;
        }
    }
}

void reclock_file_gone(struct file *f)
{
    int i;

    for (i = 0; i < RECLOCK_MAX; i++) {
        if (locks[i].used && locks[i].ofd == f) {
            drop(0, 0, 0, f);
            return;
        }
    }
}

void reclock_exit(int pid)
{
    drop(0, 0, pid, 0);
    set_waiting(pid, 0);
}
