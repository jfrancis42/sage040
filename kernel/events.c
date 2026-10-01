/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * events.c - eventfd, timerfd, signalfd, epoll and inotify.
 *
 * Five kinds of descriptor that have nothing behind them but the kernel's
 * own state, and exist so that ONE wait -- poll, select, epoll -- can
 * cover a counter, a clock, a signal and a directory as well as the
 * files and sockets it covered already. That is what event loops (libuv,
 * GLib, systemd's sd-event, nginx) are built on, and why a port that
 * uses any of them needs all five.
 *
 * EVERY WAIT HERE IS poll.c's WAIT. Whatever makes one of these ready --
 * an eventfd write, a signal, a file change -- calls poll_wake(), and
 * whatever waits sleeps on poll.c's queue. So a blocking read, a poll()
 * and an epoll_wait() are all woken by the same thing, and nothing needs
 * a queue of its own. A timer is the exception that proves it: nothing
 * happens when it expires, so it says when it will (poll_deadline) and
 * the sleep is cut to that.
 *
 * Timers are LAZY: nothing counts expirations as they pass. Whoever
 * looks works out how many there have been since it was armed.
 */
#include "events.h"
#include "dev.h"
#include "vfs.h"
#include "poll.h"
#include "pmm.h"
#include "task.h"
#include "signal.h"
#include "timer.h"
#include "wait.h"
#include "sysint.h"
#include "errno.h"
#include "string.h"

#define EV_FLAGS    (O_NONBLOCK | O_CLOEXEC)

/* A new descriptor for one of these, named as Linux names it in /proc. */
static int install(const struct file_ops *ops, void *priv, int flags,
                   const char *name)
{
    int fd = fd_install(ops, priv, O_RDWR | (flags & EV_FLAGS));
    struct file *f;

    if (fd >= 0 && (f = fd_get(fd)) != 0) {
        vfs_file_set_name(f, name);
    }
    return fd;
}

/*
 * Wait for whatever the caller is waiting for, once: -EAGAIN if it may
 * not, -EINTR for a signal, 0 to look again. `deadline` is a timer's
 * next expiry in jiffies, or 0.
 */
static int wait_once(struct file *f, u32 deadline)
{
    if (f->flags & O_NONBLOCK) {
        return -EAGAIN;
    }
    if (signal_pending(current)) {
        return -EINTR;
    }
    poll_scan_begin();
    if (deadline) {
        poll_deadline(deadline);
    }
    poll_sleep(100);
    return 0;
}

/* An anonymous inode, as Linux reports one: no type, owner-only. */
static int anon_fstat(struct file *f, struct stat *st, u32 ino)
{
    (void)f;
    memset(st, 0, sizeof(*st));
    st->st_mode = 0600;
    st->st_ino = ino;
    st->st_dev = ST_DEV_ANON;
    st->st_nlink = 1;
    if (current) {
        st->st_uid = current->euid;
        st->st_gid = current->egid;
    }
    return 0;
}

static int nbio(struct file *f, u32 request, u32 arg)
{
    if (request == FIONBIO) {
        f->flags = (*(int *)arg) ? (f->flags | O_NONBLOCK)
                                 : (f->flags & ~O_NONBLOCK);
        return 0;
    }
    return -ENOTTY;
}

/* ================================================================ */
/* eventfd                                                           */
/* ================================================================ */

/*
 * A 64-bit counter. write() adds, read() takes it all -- or, with
 * EFD_SEMAPHORE, takes one. Readable while it is not zero, writable
 * while one more would still fit below 2^64 - 1.
 */
#define EVENTFD_MAX     32
#define EVENTFD_TOP     0xfffffffffffffffeULL

struct eventfd {
    int used;
    int sema;
    u64 count;
};

static struct eventfd eventfds[EVENTFD_MAX];

static s32 efd_read(struct file *f, void *buf, u32 len)
{
    struct eventfd *e = f->priv;
    u64 v;
    int err;

    if (len < 8) {
        return -EINVAL;
    }
    while (e->count == 0) {
        if ((err = wait_once(f, 0)) < 0) {
            return err;
        }
    }
    v = e->sema ? 1 : e->count;
    e->count -= v;
    memcpy(buf, &v, 8);
    poll_wake();
    return 8;
}

static s32 efd_write(struct file *f, const void *buf, u32 len)
{
    struct eventfd *e = f->priv;
    u64 v;
    int err;

    if (len < 8) {
        return -EINVAL;
    }
    memcpy(&v, buf, 8);
    if (v == 0xffffffffffffffffULL) {
        return -EINVAL;
    }
    while (EVENTFD_TOP - e->count < v) {
        if ((err = wait_once(f, 0)) < 0) {
            return err;
        }
    }
    e->count += v;
    poll_wake();
    return 8;
}

static int efd_poll(struct file *f)
{
    struct eventfd *e = f->priv;

    return (e->count ? POLLIN : 0) | (e->count < EVENTFD_TOP ? POLLOUT : 0);
}

static int efd_close(struct file *f)
{
    ((struct eventfd *)f->priv)->used = 0;
    return 0;
}

static int efd_fstat(struct file *f, struct stat *st)
{
    return anon_fstat(f, st, 0x50000000UL |
                      (u32)((struct eventfd *)f->priv - eventfds + 1));
}

static const struct file_ops eventfd_ops = {
    efd_read,
    efd_write,
    0,                          /* not seekable */
    nbio,
    efd_close,
    efd_fstat,
    efd_poll,
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

s32 sys_eventfd2(u32 initval, int flags)
{
    int i, fd;

    if (flags & ~(EV_FLAGS | EFD_SEMAPHORE)) {
        return -EINVAL;
    }
    for (i = 0; i < EVENTFD_MAX && eventfds[i].used; i++) {
    }
    if (i == EVENTFD_MAX) {
        return -ENFILE;
    }
    eventfds[i].used = 1;
    eventfds[i].sema = (flags & EFD_SEMAPHORE) != 0;
    eventfds[i].count = initval;
    fd = install(&eventfd_ops, &eventfds[i], flags, "anon_inode:[eventfd]");
    if (fd < 0) {
        eventfds[i].used = 0;
    }
    return fd;
}

/* ================================================================ */
/* timerfd                                                           */
/* ================================================================ */

/*
 * Kept in jiffies, which is the resolution everything here has: a value
 * shorter than a tick is a tick, as setitimer's is. `next` is when it
 * next expires (armed), `interval` its period (0: once), `fired` the
 * expirations not yet read.
 */
#define TIMERFD_MAX     32
#define TICK_NS         (1000000000UL / HZ)
#define TICKS_MAX       0x3fffffffUL    /* comfortably inside s32 sums */

struct timerfd {
    int used;
    int clock;
    int armed;
    u32 next;
    u32 interval;
    u64 fired;
};

static struct timerfd timerfds[TIMERFD_MAX];

/* Count what has expired since anyone last looked. */
static void tfd_catch_up(struct timerfd *t)
{
    s32 late;

    if (!t->armed) {
        return;
    }
    late = (s32)(timer_jiffies() - t->next);
    if (late < 0) {
        return;
    }
    if (t->interval) {
        u32 n = (u32)late / t->interval + 1;

        t->fired += n;
        t->next += n * t->interval;
    } else {
        t->fired++;
        t->armed = 0;
    }
}

static s32 tfd_read(struct file *f, void *buf, u32 len)
{
    struct timerfd *t = f->priv;
    int err;

    if (len < 8) {
        return -EINVAL;
    }
    for (;;) {
        tfd_catch_up(t);
        if (t->fired) {
            break;
        }
        if ((err = wait_once(f, t->armed ? t->next : 0)) < 0) {
            return err;
        }
    }
    memcpy(buf, &t->fired, 8);
    t->fired = 0;
    return 8;
}

static int tfd_poll(struct file *f)
{
    struct timerfd *t = f->priv;

    tfd_catch_up(t);
    if (t->fired) {
        return POLLIN;
    }
    if (t->armed) {
        poll_deadline(t->next);
    }
    return 0;
}

static int tfd_close(struct file *f)
{
    ((struct timerfd *)f->priv)->used = 0;
    return 0;
}

static int tfd_fstat(struct file *f, struct stat *st)
{
    return anon_fstat(f, st, 0x51000000UL |
                      (u32)((struct timerfd *)f->priv - timerfds + 1));
}

static int tfd_ioctl(struct file *f, u32 request, u32 arg)
{
    struct timerfd *t = f->priv;

    if (request == FIONREAD) {
        tfd_catch_up(t);
        *(u32 *)arg = t->fired ? 8 : 0;
        return 0;
    }
    return nbio(f, request, arg);
}

static const struct file_ops timerfd_ops = {
    tfd_read,
    0,                          /* write: a timer is set, not written */
    0,                          /* not seekable */
    tfd_ioctl,
    tfd_close,
    tfd_fstat,
    tfd_poll,
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

s32 sys_timerfd_create(int clockid, int flags)
{
    int i, fd;

    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC &&
        clockid != CLOCK_BOOTTIME) {
        return -EINVAL;
    }
    if (flags & ~EV_FLAGS) {
        return -EINVAL;
    }
    for (i = 0; i < TIMERFD_MAX && timerfds[i].used; i++) {
    }
    if (i == TIMERFD_MAX) {
        return -ENFILE;
    }
    memset(&timerfds[i], 0, sizeof(timerfds[i]));
    timerfds[i].used = 1;
    timerfds[i].clock = clockid;
    fd = install(&timerfd_ops, &timerfds[i], flags, "anon_inode:[timerfd]");
    if (fd < 0) {
        timerfds[i].used = 0;
    }
    return fd;
}

static struct timerfd *timerfd_of(int fd)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return 0;
    }
    return f->ops == &timerfd_ops ? f->priv : 0;
}

/*
 * A duration to ticks, rounded UP (a timer may not fire early) and
 * clamped. Seconds arrive as two words so that a time64 caller's
 * value is not cut short before it is clamped.
 */
static u32 to_ticks(u32 sec_hi, u32 sec, u32 nsec)
{
    if (sec_hi || sec > TICKS_MAX / HZ) {
        return TICKS_MAX;
    }
    return sec * HZ + (nsec + TICK_NS - 1) / TICK_NS;
}

/* Ticks back to a timespec. */
static void from_ticks(u32 t, u32 *sec, u32 *nsec)
{
    *sec = t / HZ;
    *nsec = (t % HZ) * TICK_NS;
}

/* Both widths, read into one shape: seconds as hi/lo, nanoseconds. */
struct tspec {
    u32 sec_hi, sec, nsec;
    int neg;
};

struct tpair {
    struct tspec interval, value;
};

static int fetch_itimer(u32 u, int wide, struct tpair *p)
{
    int err;

    memset(p, 0, sizeof(*p));
    if (wide) {
        struct itimerspec64 k;

        if ((err = fetch(&k, u, sizeof(k))) < 0) {
            return err;
        }
        p->interval.sec_hi = (u32)((u64)k.it_interval.tv_sec >> 32);
        p->interval.sec = (u32)k.it_interval.tv_sec;
        p->interval.nsec = (u32)k.it_interval.tv_nsec;
        p->interval.neg = k.it_interval.tv_sec < 0 ||
                          k.it_interval.tv_nsec < 0 ||
                          k.it_interval.tv_nsec >= 1000000000LL;
        p->value.sec_hi = (u32)((u64)k.it_value.tv_sec >> 32);
        p->value.sec = (u32)k.it_value.tv_sec;
        p->value.nsec = (u32)k.it_value.tv_nsec;
        p->value.neg = k.it_value.tv_sec < 0 || k.it_value.tv_nsec < 0 ||
                       k.it_value.tv_nsec >= 1000000000LL;
    } else {
        struct itimerspec k;

        if ((err = fetch(&k, u, sizeof(k))) < 0) {
            return err;
        }
        p->interval.sec = k.it_interval.tv_sec;
        p->interval.nsec = k.it_interval.tv_nsec;
        p->interval.neg = (s32)k.it_interval.tv_sec < 0 ||
                          k.it_interval.tv_nsec >= 1000000000UL;
        p->value.sec = k.it_value.tv_sec;
        p->value.nsec = k.it_value.tv_nsec;
        p->value.neg = (s32)k.it_value.tv_sec < 0 ||
                       k.it_value.tv_nsec >= 1000000000UL;
    }
    return 0;
}

static int store_itimer(u32 u, int wide, struct timerfd *t)
{
    u32 left = 0, is, in, vs, vn;

    tfd_catch_up(t);
    if (t->armed) {
        s32 d = (s32)(t->next - timer_jiffies());

        left = d > 0 ? (u32)d : 1;      /* armed never reads as disarmed */
    }
    from_ticks(t->interval, &is, &in);
    from_ticks(left, &vs, &vn);
    if (wide) {
        struct itimerspec64 k;

        k.it_interval.tv_sec = is;
        k.it_interval.tv_nsec = in;
        k.it_value.tv_sec = vs;
        k.it_value.tv_nsec = vn;
        return sys_store(u, &k, sizeof(k));
    } else {
        struct itimerspec k;

        k.it_interval.tv_sec = is;
        k.it_interval.tv_nsec = in;
        k.it_value.tv_sec = vs;
        k.it_value.tv_nsec = vn;
        return sys_store(u, &k, sizeof(k));
    }
}

s32 sys_timerfd_settime(int fd, int flags, u32 unew, u32 uold, int wide)
{
    struct timerfd *t = timerfd_of(fd);
    struct tpair p;
    u32 now = timer_jiffies(), delay;
    int err;

    if (!fd_get(fd)) {
        return -EBADF;
    }
    if (!t || (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))) {
        return -EINVAL;
    }
    if ((err = fetch_itimer(unew, wide, &p)) < 0) {
        return err;
    }
    if (p.interval.neg || p.value.neg) {
        return -EINVAL;
    }
    if (uold && (err = store_itimer(uold, wide, t)) < 0) {
        return err;
    }

    t->fired = 0;
    if (!p.value.sec_hi && !p.value.sec && !p.value.nsec) {
        t->armed = 0;           /* a zero value disarms */
        poll_wake();
        return 0;
    }
    if (flags & TFD_TIMER_ABSTIME) {
        if (t->clock == CLOCK_REALTIME) {
            struct timeval tv;
            s32 ds;

            clock_get(&tv);
            ds = (s32)(p.value.sec - (u32)tv.tv_sec);
            if (p.value.sec_hi && p.value.sec_hi != 0xffffffffUL) {
                ds = p.value.sec_hi & 0x80000000UL ? -1 : 0x7fffffff;
            }
            if (ds < 0 || (ds == 0 && p.value.nsec / 1000 <= (u32)tv.tv_usec)) {
                delay = 0;
            } else {
                /* The part-second on each side, in whole ticks. */
                s32 t0 = (s32)((u32)tv.tv_usec / (1000000 / HZ));

                delay = to_ticks(0, (u32)ds, p.value.nsec);
                delay = (s32)delay - t0 > 0 ? delay - (u32)t0 : 0;
            }
        } else {
            u32 at = to_ticks(p.value.sec_hi, p.value.sec, p.value.nsec);

            delay = (s32)(at - now) > 0 ? at - now : 0;
        }
    } else {
        delay = to_ticks(p.value.sec_hi, p.value.sec, p.value.nsec);
        if (delay == 0) {
            delay = 1;
        }
    }
    t->interval = (p.interval.sec_hi || p.interval.sec || p.interval.nsec)
                  ? to_ticks(p.interval.sec_hi, p.interval.sec,
                             p.interval.nsec) : 0;
    t->next = now + delay;
    t->armed = 1;
    poll_wake();                /* a poller must learn the new deadline */
    return 0;
}

s32 sys_timerfd_gettime(int fd, u32 ucur, int wide)
{
    struct timerfd *t = timerfd_of(fd);

    if (!fd_get(fd)) {
        return -EBADF;
    }
    if (!t) {
        return -EINVAL;
    }
    return store_itimer(ucur, wide, t);
}

/* ================================================================ */
/* signalfd                                                          */
/* ================================================================ */

/*
 * A descriptor that reads the READER's pending signals, of those in its
 * mask, and takes them as it does -- which is why a program blocks them
 * first: otherwise they are delivered the ordinary way before anything
 * reads them. Inherited across fork, a signalfd reads the child's.
 */
#define SIGNALFD_MAX    16

struct signalfd {
    int used;
    u32 mask;
};

static struct signalfd signalfds[SIGNALFD_MAX];

static u32 sfd_ready(struct signalfd *s)
{
    return current ? current->sig_pending & s->mask : 0;
}

static s32 sfd_read(struct file *f, void *buf, u32 len)
{
    struct signalfd *s = f->priv;
    u32 done = 0;
    int err;

    if (len < sizeof(struct signalfd_siginfo)) {
        return -EINVAL;
    }
    while (!sfd_ready(s)) {
        if ((err = wait_once(f, 0)) < 0) {
            return err;
        }
    }
    while (len - done >= sizeof(struct signalfd_siginfo)) {
        struct signalfd_siginfo si;
        u32 ready = sfd_ready(s);
        int sig;
        u16 sr;

        if (!ready) {
            break;
        }
        for (sig = 1; !(ready & SIGMASK(sig)); sig++) {
        }
        sr = irq_save();
        current->sig_pending &= ~SIGMASK(sig);
        irq_restore(sr);

        memset(&si, 0, sizeof(si));
        si.ssi_signo = (u32)sig;
        si.ssi_code = SI_USER;
        memcpy((u8 *)buf + done, &si, sizeof(si));
        done += sizeof(si);
    }
    return (s32)done;
}

static int sfd_poll(struct file *f)
{
    return sfd_ready(f->priv) ? POLLIN : 0;
}

static int sfd_close(struct file *f)
{
    ((struct signalfd *)f->priv)->used = 0;
    return 0;
}

static int sfd_fstat(struct file *f, struct stat *st)
{
    return anon_fstat(f, st, 0x52000000UL |
                      (u32)((struct signalfd *)f->priv - signalfds + 1));
}

static const struct file_ops signalfd_ops = {
    sfd_read,
    0,                          /* write: nothing to write */
    0,                          /* not seekable */
    nbio,
    sfd_close,
    sfd_fstat,
    sfd_poll,
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

s32 sys_signalfd4(int fd, u32 umask, u32 size, int flags)
{
    u32 set[2];
    int err, i;

    if (size != SIGSET_BYTES || (flags & ~EV_FLAGS)) {
        return -EINVAL;
    }
    if ((err = fetch(set, umask, sizeof(set))) < 0) {
        return err;
    }
    set[0] &= ~SIG_UNBLOCKABLE;         /* silently, as Linux does */

    if (fd != -1) {
        struct file *f = fd_get(fd);

        if (!f) {
            return -EBADF;
        }
        if (f->ops != &signalfd_ops) {
            return -EINVAL;
        }
        ((struct signalfd *)f->priv)->mask = set[0];
        poll_wake();
        return fd;
    }
    for (i = 0; i < SIGNALFD_MAX && signalfds[i].used; i++) {
    }
    if (i == SIGNALFD_MAX) {
        return -ENFILE;
    }
    signalfds[i].used = 1;
    signalfds[i].mask = set[0];
    fd = install(&signalfd_ops, &signalfds[i], flags, "anon_inode:[signalfd]");
    if (fd < 0) {
        signalfds[i].used = 0;
    }
    return fd;
}

/* ================================================================ */
/* epoll                                                             */
/* ================================================================ */

/*
 * A set of (descriptor, file) pairs to watch. The set does not hold the
 * files open -- on Linux closing the last descriptor for a file takes it
 * out of every epoll -- so file_put() tells events_file_gone(), which
 * drops it here.
 *
 * Readiness is ASKED, not delivered: epoll_wait scans its items with
 * the same poll routine poll() uses. Level-triggered by default;
 * EPOLLET reports an item only when it has gained a bit since it was
 * last looked at, and EPOLLONESHOT stops reporting it after once until
 * EPOLL_CTL_MOD arms it again.
 */
#define EPOLL_MAX       16
#define EPOLL_ITEMS     64

struct ep_item {
    int  used;
    int  fd;
    struct file *file;
    u32  events;                /* what was asked for                  */
    u32  data[2];
    u32  last;                  /* what it was when last looked at     */
    int  disabled;              /* a oneshot that has fired            */
};

struct epoll {
    int used;
    int rotor;                  /* where the next scan starts          */
    struct ep_item items[EPOLL_ITEMS];
};

static struct epoll epolls[EPOLL_MAX];
static int ep_depth;            /* an epoll polled inside another      */

static const struct file_ops epoll_ops;

/*
 * What an item has for its caller now: the poll bits it asked for, and
 * errors and hang-ups whatever it asked. `socketish` as poll_file's.
 */
static u32 ep_check(struct ep_item *it, int *socketish, int consume)
{
    u32 r = (u32)poll_file(it->file, socketish) & 0xffff;
    u32 got, fresh;

    if (r & POLLIN)  r |= EPOLLRDNORM;
    if (r & POLLOUT) r |= EPOLLWRNORM;
    got = r & (it->events | EPOLLERR | EPOLLHUP);
    if (it->disabled || !got) {
        if (consume) {
            it->last = got;
        }
        return 0;
    }
    if (it->events & EPOLLET) {
        fresh = got & ~it->last;
        if (consume) {
            it->last = got;
        }
        return fresh ? got : 0;
    }
    return got;
}

static int ep_poll(struct file *f)
{
    struct epoll *ep = f->priv;
    int i, socketish = 0, ready = 0;

    if (ep_depth > 4) {
        return 0;
    }
    ep_depth++;
    for (i = 0; i < EPOLL_ITEMS && !ready; i++) {
        if (ep->items[i].used && ep_check(&ep->items[i], &socketish, 0)) {
            ready = 1;
        }
    }
    ep_depth--;
    return ready ? POLLIN : 0;
}

static int ep_close(struct file *f)
{
    struct epoll *ep = f->priv;

    memset(ep, 0, sizeof(*ep));
    return 0;
}

static int ep_fstat(struct file *f, struct stat *st)
{
    return anon_fstat(f, st, 0x53000000UL |
                      (u32)((struct epoll *)f->priv - epolls + 1));
}

static const struct file_ops epoll_ops = {
    0,                          /* read: epoll_wait, not read */
    0,
    0,                          /* not seekable */
    nbio,
    ep_close,
    ep_fstat,
    ep_poll,
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

s32 sys_epoll_create1(int flags)
{
    int i, fd;

    if (flags & ~O_CLOEXEC) {
        return -EINVAL;
    }
    for (i = 0; i < EPOLL_MAX && epolls[i].used; i++) {
    }
    if (i == EPOLL_MAX) {
        return -ENFILE;
    }
    memset(&epolls[i], 0, sizeof(epolls[i]));
    epolls[i].used = 1;
    fd = install(&epoll_ops, &epolls[i], flags, "anon_inode:[eventpoll]");
    if (fd < 0) {
        epolls[i].used = 0;
    }
    return fd;
}

/* Does epoll `ep` reach `target`, through epolls nested in it? */
static int ep_reaches(struct epoll *ep, struct epoll *target, int depth)
{
    int i;

    if (ep == target) {
        return 1;
    }
    if (depth > 4) {
        return 1;               /* too deep is as bad as a loop */
    }
    for (i = 0; i < EPOLL_ITEMS; i++) {
        struct ep_item *it = &ep->items[i];

        if (it->used && it->file->ops == &epoll_ops &&
            ep_reaches(it->file->priv, target, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

s32 sys_epoll_ctl(int epfd, int op, int fd, u32 uevent)
{
    struct file *ef = fd_get(epfd), *f = fd_get(fd);
    struct epoll_event ev;
    struct ep_item *it = 0, *freeslot = 0;
    struct epoll *ep;
    struct stat st;
    int i, err;

    if (!ef || !f) {
        return -EBADF;
    }
    if (ef->ops != &epoll_ops || ef == f) {
        return -EINVAL;
    }
    ep = ef->priv;
    if (op != EPOLL_CTL_DEL) {
        if ((err = fetch(&ev, uevent, sizeof(ev))) < 0) {
            return err;
        }
    }
    /* A regular file or a directory is always ready, so Linux refuses
     * to watch one rather than report it for ever. */
    if (vfs_file_stat(f, &st) == 0 &&
        (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) && !f->ops->poll) {
        return -EPERM;
    }
    for (i = 0; i < EPOLL_ITEMS; i++) {
        struct ep_item *x = &ep->items[i];

        if (x->used && x->fd == fd && x->file == f) {
            it = x;
        } else if (!x->used && !freeslot) {
            freeslot = x;
        }
    }

    switch (op) {
    case EPOLL_CTL_ADD:
        if (it) {
            return -EEXIST;
        }
        if (!freeslot) {
            return -ENOSPC;
        }
        if (f->ops == &epoll_ops && ep_reaches(f->priv, ep, 0)) {
            return -ELOOP;
        }
        memset(freeslot, 0, sizeof(*freeslot));
        freeslot->used = 1;
        freeslot->fd = fd;
        freeslot->file = f;
        freeslot->events = ev.events;
        freeslot->data[0] = ev.data[0];
        freeslot->data[1] = ev.data[1];
        break;
    case EPOLL_CTL_MOD:
        if (!it) {
            return -ENOENT;
        }
        if (ev.events & EPOLLEXCLUSIVE) {
            return -EINVAL;
        }
        it->events = ev.events;
        it->data[0] = ev.data[0];
        it->data[1] = ev.data[1];
        it->disabled = 0;
        it->last = 0;
        break;
    case EPOLL_CTL_DEL:
        if (!it) {
            return -ENOENT;
        }
        it->used = 0;
        break;
    default:
        return -EINVAL;
    }
    poll_wake();                /* a waiter has something new to look at */
    return 0;
}

static s32 ep_wait(struct epoll *ep, u32 uevents, int max, s32 timeout_ms)
{
    static struct epoll_event out[EPOLL_ITEMS];
    u32 start = timer_jiffies();
    s32 limit = timeout_ms > 0 ? poll_ms_to_ticks(timeout_ms) : 0;

    if (max > EPOLL_ITEMS) {
        max = EPOLL_ITEMS;
    }
    for (;;) {
        int n = 0, k, socketish = 0, from = ep->rotor;
        s32 slice;


        poll_scan_begin();
        ep_depth++;
        for (k = 0; k < EPOLL_ITEMS && n < max; k++) {
            int i = (from + k) % EPOLL_ITEMS;
            struct ep_item *it = &ep->items[i];
            u32 r;

            if (!it->used) {
                continue;
            }
            r = ep_check(it, &socketish, 1);
            if (!r) {
                continue;
            }
            out[n].events = r;
            out[n].data[0] = it->data[0];
            out[n].data[1] = it->data[1];
            n++;
            if (it->events & EPOLLONESHOT) {
                it->disabled = 1;
            }
            /* Next time, start after this one: a descriptor that is
             * always ready must not keep the rest from ever being
             * reported when max is small. */
            ep->rotor = (i + 1) % EPOLL_ITEMS;
        }
        ep_depth--;
        if (n) {
            int err = sys_store(uevents, out, (u32)n * sizeof(out[0]));

            return err < 0 ? err : n;
        }
        if (timeout_ms == 0) {
            return 0;
        }
        if (signal_pending(current)) {
            /* -EINTR after a handler, SA_RESTART or not, as on Linux;
             * restarted only if no handler ran (signal.c). */
            return -ERESTARTNOHAND;
        }
        slice = socketish ? 20 : 100;
        if (timeout_ms > 0) {
            s32 remaining = limit - (s32)(timer_jiffies() - start);

            if (remaining <= 0) {
                return 0;
            }
            if (remaining * (1000 / HZ) < slice) {
                slice = remaining * (1000 / HZ);
            }
        }
        poll_sleep((u32)slice);
    }
}

s32 sys_epoll_pwait(int epfd, u32 uevents, int max, s32 timeout_ms,
                    u32 umask, u32 masksize)
{
    struct file *ef = fd_get(epfd);
    u32 old = 0;
    int pushed;
    s32 r;

    if (!ef) {
        return -EBADF;
    }
    if (ef->ops != &epoll_ops || max <= 0) {
        return -EINVAL;
    }
    pushed = signal_temp_mask(umask, masksize, &old);
    if (pushed < 0) {
        return pushed;
    }
    r = ep_wait(ef->priv, uevents, max, timeout_ms);
    signal_temp_done(pushed, old, r == -ERESTARTNOHAND);
    return r;
}

/* ================================================================ */
/* inotify                                                           */
/* ================================================================ */

/*
 * Watches are on INODES, found by number: an event is reported to every
 * watch on the inode it happened to, and -- with the name -- to every
 * watch on the directory it happened in. vfs.c calls the hooks after
 * each change; they do nothing but test inotify_watching unless a watch
 * exists, so the price of the feature is one word per call.
 *
 * Each instance queues its events in one page, Linux's records back to
 * back, the name NUL-padded to a multiple of 16 as Linux pads it. A
 * full queue ends in one IN_Q_OVERFLOW and drops the rest; an event
 * identical to the last one still unread is merged into it.
 */
#define INOTIFY_MAX     8
#define WATCH_MAX       64
#define IQ_SIZE         PAGE_SIZE

struct inotify {
    int used;
    int next_wd;
    u8 *q;
    u32 n;                      /* bytes queued                        */
    u32 last;                   /* offset of the last event queued     */
    int overflowed;
};

struct iwatch {
    int used;
    struct inotify *in;
    int wd;
    u32 ino;
    u32 mask;
};

static struct inotify inotifies[INOTIFY_MAX];
static struct iwatch watches[WATCH_MAX];
int inotify_watching;

static u32 cookie_next = 1;

static void iq_put(struct inotify *in, int wd, u32 mask, u32 cookie,
                   const char *name)
{
    u32 nlen = name && *name ? ((u32)strlen(name) + 1 + 15) & ~15UL : 0;
    u32 size = sizeof(struct inotify_event) + nlen;
    struct inotify_event ev;

    if (in->n) {
        struct inotify_event *l = (struct inotify_event *)(in->q + in->last);
        const char *lname = (const char *)(l + 1);

        if (l->wd == wd && l->mask == mask && l->cookie == cookie &&
            l->len == nlen && (!nlen || strcmp(lname, name) == 0)) {
            return;             /* the same again: merged */
        }
    }
    if (in->n + size > IQ_SIZE - sizeof(struct inotify_event)) {
        if (!in->overflowed) {
            ev.wd = -1;
            ev.mask = IN_Q_OVERFLOW;
            ev.cookie = 0;
            ev.len = 0;
            in->last = in->n;
            memcpy(in->q + in->n, &ev, sizeof(ev));
            in->n += sizeof(ev);
            in->overflowed = 1;
        }
        poll_wake();
        return;
    }
    ev.wd = wd;
    ev.mask = mask;
    ev.cookie = cookie;
    ev.len = nlen;
    in->last = in->n;
    memcpy(in->q + in->n, &ev, sizeof(ev));
    if (nlen) {
        memset(in->q + in->n + sizeof(ev), 0, nlen);
        strcpy((char *)in->q + in->n + sizeof(ev), name);
    }
    in->n += size;
    poll_wake();
}

static void watch_drop(struct iwatch *w, int tell)
{
    if (tell) {
        iq_put(w->in, w->wd, IN_IGNORED, 0, 0);
    }
    w->used = 0;
    inotify_watching--;
}

/* Event `ev` (one bit, maybe with IN_ISDIR) about inode `ino`. */
static void deliver(u32 ino, u32 ev, u32 cookie, const char *name)
{
    int i;

    for (i = 0; i < WATCH_MAX; i++) {
        struct iwatch *w = &watches[i];

        if (!w->used || w->ino != ino || !(w->mask & ev & IN_ALL_EVENTS)) {
            continue;
        }
        iq_put(w->in, w->wd, ev, cookie, name);
        if (w->mask & IN_ONESHOT) {
            watch_drop(w, 1);
        }
    }
}

/* Every watch on `ino` goes: the inode has. */
static void inode_gone(u32 ino)
{
    int i;

    for (i = 0; i < WATCH_MAX; i++) {
        if (watches[i].used && watches[i].ino == ino) {
            watch_drop(&watches[i], 1);
        }
    }
}

/* The directory `path` is in, and its last component, both from the
 * absolute form of it, so that "a/../b/" names b in the right place. */
static int split(const char *path, char *dir, u32 size, char *name)
{
    char *slash;

    if (vfs_abspath(path, dir, size) < 0) {
        return -1;
    }
    for (slash = dir + strlen(dir); slash > dir && *slash != '/'; slash--) {
    }
    if (*slash != '/' || !slash[1] || strlen(slash + 1) > NAME_MAX) {
        return -1;              /* "/" is in no directory */
    }
    strcpy(name, slash + 1);
    if (slash == dir) {
        dir[1] = '\0';
    } else {
        *slash = '\0';
    }
    return 0;
}

static void to_parent(const char *path, u32 ev, u32 cookie, int isdir)
{
    char dir[PATH_MAX], name[NAME_MAX + 1];
    struct stat st;

    if (!ev || split(path, dir, sizeof(dir), name) < 0 ||
        vfs_stat(dir, &st) < 0) {
        return;
    }
    deliver(st.st_ino, ev | (isdir ? IN_ISDIR : 0), cookie, name);
}

/* One event at a time, lowest bit first, as Linux queues a combined one. */
#define EACH_BIT(m, b) for (b = 1; b && b <= (m); b <<= 1) if ((m) & b)

void inotify_path(const char *path, u32 parent, u32 self)
{
    struct stat st;
    int isdir;
    u32 b;

    if (!inotify_watching) {
        return;
    }
    if (vfs_lstat(path, &st) < 0) {
        return;
    }
    isdir = S_ISDIR(st.st_mode);
    EACH_BIT(parent, b) {
        to_parent(path, b, 0, isdir);
    }
    EACH_BIT(self, b) {
        deliver(st.st_ino, b | (isdir ? IN_ISDIR : 0), 0, 0);
    }
}

void inotify_file(struct file *f, u32 mask)
{
    char path[PATH_MAX];
    struct stat st;
    int isdir, named;
    u32 b;

    if (!inotify_watching || !f || events_owns(f)) {
        return;
    }
    if (vfs_file_stat(f, &st) < 0) {
        return;
    }
    isdir = S_ISDIR(st.st_mode);
    named = vfs_file_name(f, path, sizeof(path)) == 0 && path[0] == '/';
    EACH_BIT(mask, b) {
        deliver(st.st_ino, b | (isdir ? IN_ISDIR : 0), 0, 0);
        if (named) {
            to_parent(path, b, 0, isdir);
        }
    }
}

void inotify_look(const char *path, struct inotify_victim *v)
{
    struct stat st;

    v->valid = 0;
    if (!inotify_watching || vfs_lstat(path, &st) < 0) {
        return;
    }
    v->valid = 1;
    v->ino = st.st_ino;
    v->nlink = st.st_nlink;
    v->isdir = S_ISDIR(st.st_mode);
}

void inotify_removed(const char *path, const struct inotify_victim *v)
{
    if (!inotify_watching || !v->valid) {
        return;
    }
    to_parent(path, IN_DELETE, 0, v->isdir);
    if (v->isdir || v->nlink <= 1) {
        deliver(v->ino, IN_DELETE_SELF, 0, 0);
        inode_gone(v->ino);
    } else {
        deliver(v->ino, IN_ATTRIB, 0, 0);       /* its link count fell */
    }
}

void inotify_moved(const char *from, const char *to,
                   const struct inotify_victim *v,
                   const struct inotify_victim *replaced)
{
    u32 cookie;

    if (!inotify_watching || !v->valid) {
        return;
    }
    cookie = cookie_next++;
    if (!cookie) {
        cookie = cookie_next++;
    }
    to_parent(from, IN_MOVED_FROM, cookie, v->isdir);
    to_parent(to, IN_MOVED_TO, cookie, v->isdir);
    deliver(v->ino, IN_MOVE_SELF | (v->isdir ? IN_ISDIR : 0), 0, 0);
    if (replaced && replaced->valid && replaced->ino != v->ino &&
        (replaced->isdir || replaced->nlink <= 1)) {
        deliver(replaced->ino, IN_DELETE_SELF, 0, 0);
        inode_gone(replaced->ino);
    }
}

static s32 in_read(struct file *f, void *buf, u32 len)
{
    struct inotify *in = f->priv;
    u32 done = 0;
    int err;

    while (in->n == 0) {
        if ((err = wait_once(f, 0)) < 0) {
            return err;
        }
    }
    while (done < in->n) {
        struct inotify_event *e = (struct inotify_event *)(in->q + done);
        u32 size = sizeof(*e) + e->len;

        if (done + size > len) {
            break;
        }
        done += size;
    }
    if (done == 0) {
        return -EINVAL;         /* not even the first one fits */
    }
    memcpy(buf, in->q, done);
    memmove(in->q, in->q + done, in->n - done);
    in->n -= done;
    in->last = in->last >= done ? in->last - done : 0;
    if (in->n == 0) {
        in->overflowed = 0;
    }
    return (s32)done;
}

static int in_poll(struct file *f)
{
    return ((struct inotify *)f->priv)->n ? POLLIN : 0;
}

static int in_ioctl(struct file *f, u32 request, u32 arg)
{
    if (request == FIONREAD) {
        *(u32 *)arg = ((struct inotify *)f->priv)->n;
        return 0;
    }
    return nbio(f, request, arg);
}

static int in_close(struct file *f)
{
    struct inotify *in = f->priv;
    int i;

    for (i = 0; i < WATCH_MAX; i++) {
        if (watches[i].used && watches[i].in == in) {
            watch_drop(&watches[i], 0);
        }
    }
    pmm_free((u32)in->q);
    in->used = 0;
    return 0;
}

static int in_fstat(struct file *f, struct stat *st)
{
    return anon_fstat(f, st, 0x54000000UL |
                      (u32)((struct inotify *)f->priv - inotifies + 1));
}

static const struct file_ops inotify_ops = {
    in_read,
    0,                          /* write: nothing to write */
    0,                          /* not seekable */
    in_ioctl,
    in_close,
    in_fstat,
    in_poll,
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

s32 sys_inotify_init1(int flags)
{
    int i, fd;
    u32 page;

    if (flags & ~EV_FLAGS) {
        return -EINVAL;
    }
    for (i = 0; i < INOTIFY_MAX && inotifies[i].used; i++) {
    }
    if (i == INOTIFY_MAX) {
        return -EMFILE;
    }
    page = pmm_alloc();
    if (!page) {
        return -ENOMEM;
    }
    memset(&inotifies[i], 0, sizeof(inotifies[i]));
    inotifies[i].used = 1;
    inotifies[i].q = (u8 *)page;
    fd = install(&inotify_ops, &inotifies[i], flags, "anon_inode:inotify");
    if (fd < 0) {
        pmm_free(page);
        inotifies[i].used = 0;
    }
    return fd;
}

static struct inotify *inotify_of(int fd, int *err)
{
    struct file *f = fd_get(fd);

    *err = !f ? -EBADF : f->ops != &inotify_ops ? -EINVAL : 0;
    return *err ? 0 : f->priv;
}

s32 sys_inotify_add_watch(int fd, u32 upath, u32 mask)
{
    char path[PATH_MAX];
    struct inotify *in;
    struct stat st;
    int err, i, slot = -1;

    if (!(in = inotify_of(fd, &err))) {
        return err;
    }
    if (!(mask & IN_ALL_EVENTS) ||
        ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE))) {
        return -EINVAL;
    }
    if ((err = fetch_str(path, upath, sizeof(path))) < 0) {
        return err;
    }
    err = (mask & IN_DONT_FOLLOW) ? vfs_lstat(path, &st) : vfs_stat(path, &st);
    if (err < 0) {
        return err;
    }
    if ((mask & IN_ONLYDIR) && !S_ISDIR(st.st_mode)) {
        return -ENOTDIR;
    }
    if ((err = vfs_may(path, R_OK)) < 0) {
        return err;
    }
    for (i = 0; i < WATCH_MAX; i++) {
        struct iwatch *w = &watches[i];

        if (w->used && w->in == in && w->ino == st.st_ino) {
            if (mask & IN_MASK_CREATE) {
                return -EEXIST;
            }
            w->mask = (mask & IN_MASK_ADD) ? (w->mask | mask) : mask;
            return w->wd;
        }
        if (!w->used && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -ENOSPC;
    }
    watches[slot].used = 1;
    watches[slot].in = in;
    watches[slot].wd = ++in->next_wd;
    watches[slot].ino = st.st_ino;
    watches[slot].mask = mask & ~IN_MASK_ADD;
    inotify_watching++;
    return watches[slot].wd;
}

s32 sys_inotify_rm_watch(int fd, int wd)
{
    struct inotify *in;
    int err, i;

    if (!(in = inotify_of(fd, &err))) {
        return err;
    }
    for (i = 0; i < WATCH_MAX; i++) {
        if (watches[i].used && watches[i].in == in && watches[i].wd == wd) {
            watch_drop(&watches[i], 1);
            return 0;
        }
    }
    return -EINVAL;
}

/* ================================================================ */
/* Shared                                                            */
/* ================================================================ */

int events_owns(struct file *f)
{
    return f && f->ops && (f->ops == &eventfd_ops || f->ops == &timerfd_ops ||
                           f->ops == &signalfd_ops || f->ops == &inotify_ops);
}

void events_file_gone(struct file *f)
{
    int e, i;

    for (e = 0; e < EPOLL_MAX; e++) {
        if (!epolls[e].used) {
            continue;
        }
        for (i = 0; i < EPOLL_ITEMS; i++) {
            if (epolls[e].items[i].used && epolls[e].items[i].file == f) {
                epolls[e].items[i].used = 0;
            }
        }
    }
}
