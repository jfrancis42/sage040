/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mqueue.c - POSIX message queues: mq_open and the rest, Linux's calls.
 *
 * A queue is a name, an owner and a mode, a capacity (mq_maxmsg
 * messages of at most mq_msgsize bytes each), and the messages, kept in
 * PRIORITY order -- highest first, first in first out within one
 * priority -- which is the whole difference from a pipe. A descriptor
 * refers to one; mq_unlink removes the name, and the queue itself goes
 * when the last descriptor on it closes, as an unlinked file does.
 *
 * THE NAME IS LINUX'S SYSTEM CALL NAME: no leading slash. A program
 * says mq_open("/jobs", ...) and the C library strips the slash, as
 * glibc's does, so "jobs" arrives here; one with a slash in it is
 * refused (EACCES, as Linux's mqueue filesystem refuses it).
 *
 * LIMITS ARE LINUX'S DEFAULTS: at most 10 messages of 8192 bytes for
 * anyone but root (fs.mqueue.msg_max and msgsize_max), both the default
 * attributes when none are given; root may ask for up to 64 of 65536.
 * Each message is held in the pages it needs while it waits, so an
 * empty queue costs nothing but its entry here.
 *
 * mq_timedsend and mq_timedreceive block while the queue is full or
 * empty -- O_NONBLOCK says EAGAIN instead -- for no longer than an
 * ABSOLUTE CLOCK_REALTIME time, if given (ETIMEDOUT), and a signal ends
 * the wait (EINTR). mq_notify registers ONE process for a signal when a
 * message arrives in an empty queue and nobody is already waiting to
 * receive it; the registration is used up by that, as POSIX says, and
 * SIGEV_THREAD -- which glibc builds in user space over netlink -- is
 * not offered. A descriptor polls readable while there are messages and
 * writable while there is room.
 */

#include "mqueue.h"
#include "task.h"
#include "vfs.h"
#include "dev.h"
#include "wait.h"
#include "signal.h"
#include "pmm.h"
#include "timer.h"
#include "uaccess.h"
#include "sysint.h"
#include "poll.h"
#include "errno.h"
#include "string.h"

#define MQ_MAX          16      /* queues on the machine at once        */
#define MQ_NAME_MAX     63
#define MQ_SLOTS        64      /* the most messages root may ask for   */
#define MQ_PRIO_MAX     32768
#define MQ_USER_MAXMSG  10      /* Linux's fs.mqueue.msg_max            */
#define MQ_USER_MSGSIZE 8192    /* ... and msgsize_max                  */
#define MQ_ROOT_MSGSIZE 65536
#define MQ_PAGES        (MQ_ROOT_MSGSIZE / PAGE_SIZE)

struct mqmsg {
    u32 prio;
    u32 len;
    u32 page[MQ_PAGES];         /* as many as len needs; 0 past them   */
};

struct mqueue {
    int  used;
    int  linked;                /* its name still finds it             */
    int  refs;                  /* descriptors open on it              */
    char name[MQ_NAME_MAX + 1];
    u32  uid, gid, mode;
    u32  maxmsg, msgsize;
    u32  count;
    struct mqmsg msg[MQ_SLOTS]; /* [0] is the next to be received      */
    u32  ino;
    struct waitq rq, wq;
    int  receivers;             /* blocked in receive right now        */
    struct task *notify;        /* registered by mq_notify, or null    */
    int  notify_sig;            /* 0: SIGEV_NONE                       */
    u32  notify_value;
};

static struct mqueue queues[MQ_MAX];
static u32 next_ino = 1;

/* Linux/m68k's struct mq_attr: four longs and four reserved. */
struct kmq_attr {
    s32 mq_flags, mq_maxmsg, mq_msgsize, mq_curmsgs;
    s32 reserved[4];
};

/* Linux/m68k's struct sigevent: 64 bytes, of which three fields count. */
struct ksigevent {
    u32 sigev_value;
    int sigev_signo;
    int sigev_notify;
    int pad[13];
};
#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1

static void free_msg(struct mqmsg *m)
{
    u32 i;

    for (i = 0; i < MQ_PAGES; i++) {
        if (m->page[i]) {
            pmm_free(m->page[i]);
            m->page[i] = 0;
        }
    }
    m->len = 0;
}

static void mq_free(struct mqueue *q)
{
    u32 i;

    for (i = 0; i < q->count; i++) {
        free_msg(&q->msg[i]);
    }
    memset(q, 0, sizeof(*q));
}

static struct mqueue *by_name(const char *name)
{
    int i;

    for (i = 0; i < MQ_MAX; i++) {
        if (queues[i].used && queues[i].linked &&
            strcmp(queues[i].name, name) == 0) {
            return &queues[i];
        }
    }
    return 0;
}

/* --- the descriptor --------------------------------------------------- */

static int mq_close(struct file *f)
{
    struct mqueue *q = f->priv;

    if (q->notify == current) {
        q->notify = 0;          /* a registration lasts while it is open */
    }
    if (--q->refs == 0 && !q->linked) {
        mq_free(q);
    }
    return 0;
}

static int mq_fstat(struct file *f, struct stat *st)
{
    struct mqueue *q = f->priv;

    st->st_mode = S_IFREG | (q->mode & 0777);
    st->st_uid = q->uid;
    st->st_gid = q->gid;
    st->st_ino = q->ino;
    st->st_nlink = q->linked ? 1 : 0;
    st->st_size = 0;
    return 0;
}

static int mq_poll(struct file *f)
{
    struct mqueue *q = f->priv;
    int r = 0;

    if (q->count > 0) {
        r |= POLLIN | POLLRDNORM;
    }
    if (q->count < q->maxmsg) {
        r |= POLLOUT | POLLWRNORM;
    }
    return r;
}

static s32 mq_rw(struct file *f, void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EBADF;              /* mq_timedsend and receive, not this  */
}

static const struct file_ops mq_ops = {
    (s32 (*)(struct file *, void *, u32))mq_rw,
    (s32 (*)(struct file *, const void *, u32))mq_rw,
    0,                          /* lseek    */
    0,                          /* ioctl    */
    mq_close,
    mq_fstat,
    mq_poll,
    0,                          /* truncate */
    0,                          /* mmap     */
};

static struct mqueue *queue_of(int fd, struct file **fp)
{
    struct file *f = fd_get(fd);

    if (!f || f->ops != &mq_ops) {
        return 0;
    }
    if (fp) {
        *fp = f;
    }
    return f->priv;
}

/* --- the calls -------------------------------------------------------- */

/* What a program may do with a queue: the owner's, group's or others'
 * bits, as for a file; root may do anything. */
static int may(const struct mqueue *q, int acc)
{
    u32 bits;

    if (current->euid == 0) {
        return 1;
    }
    bits = current->euid == q->uid ? (q->mode >> 6) :
           current->egid == q->gid ? (q->mode >> 3) : q->mode;
    if ((acc == O_RDONLY || acc == O_RDWR) && !(bits & 4)) {
        return 0;
    }
    if ((acc == O_WRONLY || acc == O_RDWR) && !(bits & 2)) {
        return 0;
    }
    return 1;
}

s32 sys_mq_open(u32 uname, int oflag, u32 mode, u32 uattr)
{
    char name[MQ_NAME_MAX + 2];
    struct mqueue *q;
    struct kmq_attr a;
    int n, i, acc = oflag & O_ACCMODE, fd;

    n = fetch_str(name, uname, sizeof(name));
    if (n < 0) {
        return n;
    }
    if (n > MQ_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    if (n == 0) {
        return -ENOENT;
    }
    if (strchr(name, '/')) {
        return -EACCES;
    }
    if (acc == O_ACCMODE) {
        return -EINVAL;
    }
    q = by_name(name);
    if (q) {
        if ((oflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
            return -EEXIST;
        }
        if (!may(q, acc)) {
            return -EACCES;
        }
    } else {
        if (!(oflag & O_CREAT)) {
            return -ENOENT;
        }
        a.mq_maxmsg = MQ_USER_MAXMSG;
        a.mq_msgsize = MQ_USER_MSGSIZE;
        if (uattr) {
            int err = fetch(&a, uattr, sizeof(a));

            if (err < 0) {
                return err;
            }
            if (a.mq_maxmsg <= 0 || a.mq_msgsize <= 0) {
                return -EINVAL;
            }
            if (current->euid != 0
                ? (a.mq_maxmsg > MQ_USER_MAXMSG || a.mq_msgsize > MQ_USER_MSGSIZE)
                : (a.mq_maxmsg > MQ_SLOTS || a.mq_msgsize > MQ_ROOT_MSGSIZE)) {
                return -EINVAL;
            }
        }
        for (i = 0; i < MQ_MAX && queues[i].used; i++) {
        }
        if (i == MQ_MAX) {
            return -ENOSPC;
        }
        q = &queues[i];
        memset(q, 0, sizeof(*q));
        q->used = 1;
        q->linked = 1;
        strcpy(q->name, name);
        q->uid = current->euid;
        q->gid = current->egid;
        q->mode = mode & 0777 & ~current->umask;
        q->maxmsg = (u32)a.mq_maxmsg;
        q->msgsize = (u32)a.mq_msgsize;
        q->ino = next_ino++;
    }
    fd = fd_install(&mq_ops, q, oflag & (O_ACCMODE | O_NONBLOCK | O_CLOEXEC));
    if (fd < 0) {
        if (q->refs == 0) {
            mq_free(q);
        }
        return fd;
    }
    q->refs++;
    return fd;
}

s32 sys_mq_unlink(u32 uname)
{
    char name[MQ_NAME_MAX + 2];
    struct mqueue *q;
    int n = fetch_str(name, uname, sizeof(name));

    if (n < 0) {
        return n;
    }
    if (n > MQ_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    q = by_name(name);
    if (!q) {
        return -ENOENT;
    }
    if (current->euid != 0 && current->euid != q->uid) {
        return -EACCES;
    }
    q->linked = 0;
    if (q->refs == 0) {
        mq_free(q);
    }
    return 0;
}

/*
 * How long until an absolute CLOCK_REALTIME time, in ms: -1 for no
 * limit, 0 if it has passed. A tv_nsec out of range is EINVAL, as Linux
 * says -- but only when the call would have had to wait.
 */
static s32 until(u32 uts, int time64, s32 *ms)
{
    s64 sec;
    s32 nsec;
    struct timeval now;
    s64 left;

    *ms = -1;
    if (!uts) {
        return 0;
    }
    if (time64) {
        struct { s64 sec; s32 pad; s32 nsec; } t;
        int err = fetch(&t, uts, sizeof(t));

        if (err < 0) {
            return err;
        }
        sec = t.sec;
        nsec = t.nsec;
    } else {
        struct timespec t;
        int err = fetch(&t, uts, sizeof(t));

        if (err < 0) {
            return err;
        }
        sec = (s64)(u32)t.tv_sec;   /* unsigned: this kernel's time_t */
        nsec = t.tv_nsec;
    }
    if (nsec < 0 || nsec >= 1000000000) {
        return -EINVAL;
    }
    clock_get(&now);
    left = (sec - (s64)(u32)now.tv_sec) * 1000 +
           (nsec / 1000000 - (s32)now.tv_usec / 1000);
    *ms = left <= 0 ? 0 : left > 0x7fffffff ? 0x7fffffff : (s32)left;
    return 0;
}

/* Sleep on `w` for at most `*ms` (-1: no limit). -ETIMEDOUT or -EINTR,
 * or 0 to look again. */
static s32 wait_turn(struct waitq *w, s32 *ms)
{
    u32 t0;

    if (signal_pending(current)) {
        return -EINTR;
    }
    if (*ms < 0) {
        sleep_on(w);
        return signal_pending(current) ? -EINTR : 0;
    }
    if (*ms == 0) {
        return -ETIMEDOUT;
    }
    t0 = timer_jiffies();
    sleep_on_timeout(w, (u32)*ms);
    *ms -= (s32)((timer_jiffies() - t0) * (1000 / HZ));
    if (*ms < 0) {
        *ms = 0;
    }
    return signal_pending(current) ? -EINTR : 0;
}

s32 sys_mq_timedsend(int fd, u32 ubuf, u32 len, u32 prio, u32 uts, int time64)
{
    struct file *f;
    struct mqueue *q = queue_of(fd, &f);
    struct mqmsg m;
    s32 ms, err;
    u32 done, i, at;

    if (!q || (f->flags & O_ACCMODE) == O_RDONLY) {
        return -EBADF;
    }
    if (len > q->msgsize) {
        return -EMSGSIZE;
    }
    if (prio >= MQ_PRIO_MAX) {
        return -EINVAL;
    }
    if (q->count >= q->maxmsg && (f->flags & O_NONBLOCK)) {
        return -EAGAIN;
    }

    /* The message, into pages of its own, FIRST: copying can fault and
     * sleep, and nothing may sleep between finding room and taking it. */
    memset(&m, 0, sizeof(m));
    m.prio = prio;
    m.len = len;
    for (done = 0, i = 0; done < len; i++, done += PAGE_SIZE) {
        u32 n = len - done < PAGE_SIZE ? len - done : PAGE_SIZE;

        m.page[i] = pmm_alloc();
        if (!m.page[i]) {
            free_msg(&m);
            return -ENOMEM;
        }
        if (fetch((void *)m.page[i], ubuf + done, n) < 0) {
            free_msg(&m);
            return -EFAULT;
        }
    }

    if (q->count >= q->maxmsg) {
        if (f->flags & O_NONBLOCK) {
            free_msg(&m);
            return -EAGAIN;
        }
        err = until(uts, time64, &ms);
        while (err == 0 && q->count >= q->maxmsg) {
            err = wait_turn(&q->wq, &ms);
        }
        if (err < 0) {
            free_msg(&m);
            return err;
        }
    }

    /* After every message of its priority or higher. */
    for (at = 0; at < q->count && q->msg[at].prio >= prio; at++) {
    }
    memmove(&q->msg[at + 1], &q->msg[at], (q->count - at) * sizeof(m));
    q->msg[at] = m;
    q->count++;

    /* Empty to not empty, nobody waiting to take it: the registrant is
     * told, and the registration is spent. */
    if (q->count == 1 && q->notify && q->receivers == 0) {
        struct task *t = q->notify;

        q->notify = 0;
        if (q->notify_sig) {
            signal_send_info(t, q->notify_sig, SI_MESGQ, q->notify_value);
        }
    }
    wake_all(&q->rq);
    poll_wake();
    return 0;
}

s32 sys_mq_timedreceive(int fd, u32 ubuf, u32 len, u32 uprio, u32 uts,
                        int time64)
{
    struct file *f;
    struct mqueue *q = queue_of(fd, &f);
    struct mqmsg m;
    s32 ms = -1, err;
    u32 done, i;

    if (!q || (f->flags & O_ACCMODE) == O_WRONLY) {
        return -EBADF;
    }
    if (len < q->msgsize) {
        return -EMSGSIZE;
    }
    if (q->count == 0) {
        if (f->flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        err = until(uts, time64, &ms);
        if (err < 0) {
            return err;
        }
        q->receivers++;
        while (q->count == 0) {
            err = wait_turn(&q->rq, &ms);
            if (err < 0) {
                q->receivers--;
                return err;
            }
        }
        q->receivers--;
    }

    m = q->msg[0];
    q->count--;
    memmove(&q->msg[0], &q->msg[1], q->count * sizeof(m));
    memset(&q->msg[q->count], 0, sizeof(m));
    wake_all(&q->wq);
    poll_wake();

    for (done = 0, i = 0; done < m.len; i++, done += PAGE_SIZE) {
        u32 n = m.len - done < PAGE_SIZE ? m.len - done : PAGE_SIZE;

        if (sys_store(ubuf + done, (void *)m.page[i], n) < 0) {
            free_msg(&m);
            return -EFAULT;
        }
    }
    len = m.len;                /* free_msg forgets it */
    free_msg(&m);
    if (uprio && sys_store(uprio, &m.prio, sizeof(m.prio)) < 0) {
        return -EFAULT;
    }
    return (s32)len;
}

s32 sys_mq_notify(int fd, u32 uev)
{
    struct mqueue *q = queue_of(fd, 0);
    struct ksigevent ev;

    if (!q) {
        return -EBADF;
    }
    if (!uev) {
        if (q->notify == current) {
            q->notify = 0;
        }
        return 0;
    }
    if (fetch(&ev, uev, sizeof(ev)) < 0) {
        return -EFAULT;
    }
    if (ev.sigev_notify != SIGEV_SIGNAL && ev.sigev_notify != SIGEV_NONE) {
        return -EINVAL;
    }
    if (ev.sigev_notify == SIGEV_SIGNAL &&
        (ev.sigev_signo <= 0 || ev.sigev_signo >= NSIG)) {
        return -EINVAL;
    }
    if (q->notify && q->notify != current) {
        return -EBUSY;
    }
    q->notify = current;
    q->notify_sig = ev.sigev_notify == SIGEV_SIGNAL ? ev.sigev_signo : 0;
    q->notify_value = ev.sigev_value;
    return 0;
}

s32 sys_mq_getsetattr(int fd, u32 unew, u32 uold)
{
    struct file *f;
    struct mqueue *q = queue_of(fd, &f);
    struct kmq_attr a;

    if (!q) {
        return -EBADF;
    }
    memset(&a, 0, sizeof(a));
    a.mq_flags = f->flags & O_NONBLOCK;
    a.mq_maxmsg = (s32)q->maxmsg;
    a.mq_msgsize = (s32)q->msgsize;
    a.mq_curmsgs = (s32)q->count;
    if (unew) {
        struct kmq_attr n;

        if (fetch(&n, unew, sizeof(n)) < 0) {
            return -EFAULT;
        }
        if (n.mq_flags & ~O_NONBLOCK) {
            return -EINVAL;
        }
        f->flags = (f->flags & ~O_NONBLOCK) | (n.mq_flags & O_NONBLOCK);
    }
    if (uold && sys_store(uold, &a, sizeof(a)) < 0) {
        return -EFAULT;
    }
    return 0;
}

/* A task is gone: a registration it held goes with it. */
void mq_task_exit(struct task *t)
{
    int i;

    for (i = 0; i < MQ_MAX; i++) {
        if (queues[i].used && queues[i].notify == t) {
            queues[i].notify = 0;
        }
    }
}
