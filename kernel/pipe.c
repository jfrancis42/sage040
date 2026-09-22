/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pipe.c - pipes.
 *
 * A ring of PIPE_SIZE bytes with a reading end and a writing end, each
 * an ordinary open file, so read(), write(), close(), poll() and dup2()
 * work on them with nothing special anywhere else. Every end open
 * anywhere is counted, which is what end of file and SIGPIPE are made
 * of: a reader of an empty pipe that no one can write to any more has
 * reached the end, and a writer to a pipe that no one can read is told
 * so with SIGPIPE.
 *
 * Counted per OPEN FILE, not per descriptor. dup() and spawn() share a
 * struct file and add references to it, and the end is only closed when
 * the last reference goes -- which is exactly "the last descriptor for
 * this end, in any task, was closed".
 */
#include "pipe.h"
#include "vfs.h"
#include "dev.h"
#include "task.h"
#include "wait.h"
#include "signal.h"
#include "poll.h"
#include "pmm.h"
#include "errno.h"
#include "string.h"

#define PIPE_MAX    64          /* rings: a socket pair uses two */

struct pipe {
    int  used;
    u8  *buf;                   /* a page of its own: PIPE_SIZE bytes */
    u32  head;                  /* next byte to read                  */
    u32  count;                 /* bytes held                         */
    int  readers;               /* open files on each end             */
    int  writers;
    struct waitq rq;            /* waiting for something to read      */
    struct waitq wq;            /* waiting for room to write          */
};

static struct pipe pipes[PIPE_MAX];

static void ring_put(struct pipe *p, int reader);

static void wake_everyone(struct pipe *p)
{
    wake_all(&p->rq);
    wake_all(&p->wq);
    poll_wake();
}

/*
 * The ring's own read and write. Pipes use them, and so do socket pairs,
 * which are a pipe in each direction.
 */
#define RW_NONBLOCK 1           /* fail with EAGAIN rather than wait   */
#define RW_PEEK     2           /* read without taking                  */
#define RW_NOSIGNAL 4           /* EPIPE without SIGPIPE                */

static s32 ring_read(struct pipe *p, void *buf, u32 len, int how)
{
    u8 *out = buf;
    u32 n = 0;

    if (len == 0) {
        return 0;
    }
    for (;;) {
        if (p->count > 0) {
            u32 at = p->head;

            while (n < len && n < p->count) {
                out[n++] = p->buf[at];
                at = (at + 1) % PIPE_SIZE;
            }
            if (!(how & RW_PEEK)) {
                p->head = at;
                p->count -= n;
                wake_everyone(p);
            }
            return (s32)n;
        }
        if (p->writers == 0) {
            return 0;                   /* end of file */
        }
        if (how & RW_NONBLOCK) {
            return -EAGAIN;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        sleep_on(&p->rq);
    }
}

static s32 pipe_read(struct file *f, void *buf, u32 len)
{
    return ring_read(f->priv, buf, len,
                     (f->flags & O_NONBLOCK) ? RW_NONBLOCK : 0);
}

static s32 ring_write(struct pipe *p, const void *buf, u32 len, int how)
{
    const u8 *in = buf;
    u32 done = 0;

    while (done < len) {
        u32 room, want;

        if (p->readers == 0) {
            /* Nobody will ever read it. The signal is the usual way a
             * program finds out; EPIPE is for one that ignores it. */
            if (!(how & RW_NOSIGNAL)) {
                signal_send(current, SIGPIPE);
            }
            return done > 0 ? (s32)done : -EPIPE;
        }
        room = PIPE_SIZE - p->count;
        /*
         * A write of PIPE_BUF or less goes in whole or not at all, so
         * two writers' small messages never interleave. A bigger one may
         * go in pieces as room appears, like everywhere else.
         */
        want = len - done;
        if (len <= PIPE_BUF ? room < want : room == 0) {
            if (how & RW_NONBLOCK) {
                return done > 0 ? (s32)done : -EAGAIN;
            }
            if (signal_pending(current)) {
                return done > 0 ? (s32)done : -EINTR;
            }
            sleep_on(&p->wq);
            continue;
        }
        if (want > room) {
            want = room;
        }
        while (want--) {
            p->buf[(p->head + p->count) % PIPE_SIZE] = in[done++];
            p->count++;
        }
        wake_everyone(p);
    }
    return (s32)done;
}

static s32 pipe_write(struct file *f, const void *buf, u32 len)
{
    return ring_write(f->priv, buf, len,
                      (f->flags & O_NONBLOCK) ? RW_NONBLOCK : 0);
}

static int pipe_close(struct file *f)
{
    /* A reader waiting on the last writer, or a writer on the last
     * reader, is woken by ring_put and finds out now. */
    ring_put(f->priv, (f->flags & O_ACCMODE) != O_WRONLY);
    return 0;
}

static int pipe_fstat(struct file *f, struct stat *st)
{
    struct pipe *p = f->priv;

    st->st_mode = S_IFIFO;
    st->st_size = p->count;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static int pipe_ioctl(struct file *f, u32 request, u32 arg)
{
    struct pipe *p = f->priv;

    if (request == FIONREAD) {
        if (arg) {
            *(u32 *)arg = p->count;
        }
        return 0;
    }
    if (request == FIONBIO) {
        f->flags = (*(int *)arg) ? (f->flags | O_NONBLOCK)
                                 : (f->flags & ~O_NONBLOCK);
        return 0;
    }
    return -ENOTTY;
}

static int pipe_poll(struct file *f)
{
    struct pipe *p = f->priv;

    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        if (p->readers == 0) {
            return POLLERR;
        }
        return p->count < PIPE_SIZE ? POLLOUT : 0;
    }
    if (p->count > 0) {
        return POLLIN;
    }
    return p->writers == 0 ? POLLHUP : 0;
}

static const struct file_ops pipe_ops = {
    pipe_read,
    pipe_write,
    0,                          /* not seekable */
    pipe_ioctl,
    pipe_close,
    pipe_fstat,
    pipe_poll,
    0,                          /* truncate: nothing to truncate */
};

/* A new ring with one reader and one writer counted, or null. */
static struct pipe *ring_alloc(void)
{
    int i;

    for (i = 0; i < PIPE_MAX; i++) {
        struct pipe *p = &pipes[i];

        if (!p->used) {
            memset(p, 0, sizeof(*p));
            p->buf = (u8 *)pmm_alloc();
            if (!p->buf) {
                return 0;
            }
            p->used = 1;
            p->readers = 1;
            p->writers = 1;
            return p;
        }
    }
    return 0;
}

/* Drop one reader or writer, and free the ring when it has neither. */
static void ring_put(struct pipe *p, int reader)
{
    if (reader) {
        p->readers--;
    } else {
        p->writers--;
    }
    wake_everyone(p);
    if (p->readers == 0 && p->writers == 0) {
        pmm_free((u32)p->buf);
        p->used = 0;
    }
}

int pipe_create(int fds[2], int flags)
{
    struct pipe *p;
    int r, w;

    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) {
        return -EINVAL;
    }
    p = ring_alloc();
    if (!p) {
        return -ENFILE;
    }

    r = fd_install(&pipe_ops, p, O_RDONLY | flags);
    if (r < 0) {
        pmm_free((u32)p->buf);
        p->used = 0;
        return r;
    }
    w = fd_install(&pipe_ops, p, O_WRONLY | flags);
    if (w < 0) {
        p->writers = 0;         /* so closing the reader frees it */
        fd_close(r);
        return w;
    }
    fds[0] = r;
    fds[1] = w;
    return 0;
}

/* --- socket pairs ---------------------------------------------------- */

/*
 * socketpair(AF_UNIX, SOCK_STREAM): two connected ends, each reading one
 * ring and writing the other. A Unix stream socket pair and two pipes
 * are the same object, which is why this lives here and not in net/.
 */
struct uend {
    int used;
    struct pipe *rx, *tx;
    int shut_rd, shut_wr;       /* shutdown() has dropped that side */
};

static struct uend uends[PIPE_MAX];

static s32 usock_read(struct file *f, void *buf, u32 len)
{
    return usock_recv(f, buf, len, 0);
}

static s32 usock_write(struct file *f, const void *buf, u32 len)
{
    return usock_send(f, buf, len, 0);
}

static void usock_close_raw(struct uend *u)
{
    if (!u->shut_rd) {
        ring_put(u->rx, 1);
    }
    if (!u->shut_wr) {
        ring_put(u->tx, 0);
    }
    u->used = 0;
}

static int usock_close(struct file *f)
{
    struct uend *u = f->priv;

    if (!u->shut_rd) {
        ring_put(u->rx, 1);
    }
    if (!u->shut_wr) {
        ring_put(u->tx, 0);
    }
    u->used = 0;
    return 0;
}

static int usock_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFSOCK;
    st->st_size = 0;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static int usock_ioctl(struct file *f, u32 request, u32 arg)
{
    struct uend *u = f->priv;

    if (request == FIONREAD) {
        if (arg) {
            *(u32 *)arg = u->shut_rd ? 0 : u->rx->count;
        }
        return 0;
    }
    if (request == FIONBIO) {
        f->flags = (*(int *)arg) ? (f->flags | O_NONBLOCK)
                                 : (f->flags & ~O_NONBLOCK);
        return 0;
    }
    return -ENOTTY;
}

static int usock_poll(struct file *f)
{
    struct uend *u = f->priv;
    int r = 0;

    if (u->shut_rd || u->rx->count > 0) {
        r |= POLLIN;
    } else if (u->rx->writers == 0) {
        r |= POLLIN | POLLHUP;
    }
    if (u->shut_wr || u->tx->readers == 0) {
        r |= POLLERR;
    } else if (u->tx->count < PIPE_SIZE) {
        r |= POLLOUT;
    }
    return r;
}

static const struct file_ops usock_ops = {
    usock_read,
    usock_write,
    0,
    usock_ioctl,
    usock_close,
    usock_fstat,
    usock_poll,
    0,                          /* truncate: nothing to truncate */
};

int usock_is(struct file *f)
{
    return f && f->ops == &usock_ops;
}

s32 usock_send(struct file *f, const void *buf, u32 len, int flags)
{
    struct uend *u = f->priv;
    int how = 0;

    if (u->shut_wr) {
        if (!(flags & MSG_NOSIGNAL)) {
            signal_send(current, SIGPIPE);
        }
        return -EPIPE;
    }
    if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
        how |= RW_NONBLOCK;
    }
    if (flags & MSG_NOSIGNAL) {
        how |= RW_NOSIGNAL;
    }
    return ring_write(u->tx, buf, len, how);
}

s32 usock_recv(struct file *f, void *buf, u32 len, int flags)
{
    struct uend *u = f->priv;
    int how = 0;

    if (u->shut_rd) {
        return 0;
    }
    if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
        how |= RW_NONBLOCK;
    }
    if (flags & MSG_PEEK) {
        how |= RW_PEEK;
    }
    if (flags & MSG_WAITALL) {
        s32 done = 0;

        while ((u32)done < len) {
            s32 n = ring_read(u->rx, (u8 *)buf + done, len - (u32)done, how);

            if (n <= 0) {
                return done > 0 ? done : n;
            }
            done += n;
        }
        return done;
    }
    return ring_read(u->rx, buf, len, how);
}

int usock_shutdown(struct file *f, int how)
{
    struct uend *u = f->priv;

    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
        return -EINVAL;
    }
    /* Dropping our side of a ring is what the peer sees: end of file
     * for SHUT_WR, EPIPE for SHUT_RD. */
    if ((how == SHUT_RD || how == SHUT_RDWR) && !u->shut_rd) {
        u->shut_rd = 1;
        ring_put(u->rx, 1);
    }
    if ((how == SHUT_WR || how == SHUT_RDWR) && !u->shut_wr) {
        u->shut_wr = 1;
        ring_put(u->tx, 0);
    }
    return 0;
}

static struct uend *uend_alloc(void)
{
    int i;

    for (i = 0; i < PIPE_MAX; i++) {
        if (!uends[i].used) {
            memset(&uends[i], 0, sizeof(uends[i]));
            uends[i].used = 1;
            return &uends[i];
        }
    }
    return 0;
}

int usock_pair(int type, int fds[2])
{
    struct pipe *ab, *ba;
    struct uend *a, *b;
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int fa, fb;

    if ((type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != SOCK_STREAM) {
        return -EPROTONOSUPPORT;    /* no datagram pairs: see uapi.h */
    }
    ab = ring_alloc();
    ba = ring_alloc();
    a = uend_alloc();
    b = uend_alloc();
    if (!ab || !ba || !a || !b) {
        if (ab) { ring_put(ab, 1); ring_put(ab, 0); }
        if (ba) { ring_put(ba, 1); ring_put(ba, 0); }
        if (a) { a->used = 0; }
        if (b) { b->used = 0; }
        return -ENFILE;
    }
    a->rx = ba;
    a->tx = ab;
    b->rx = ab;
    b->tx = ba;
    fa = fd_install(&usock_ops, a, O_RDWR | flags);
    if (fa < 0) {
        usock_close_raw(a);
        usock_close_raw(b);
        return fa;
    }
    fb = fd_install(&usock_ops, b, O_RDWR | flags);
    if (fb < 0) {
        fd_close(fa);
        usock_close_raw(b);
        return fb;
    }
    fds[0] = fa;
    fds[1] = fb;
    return 0;
}
