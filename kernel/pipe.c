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

#define PIPE_MAX    32

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

static void wake_everyone(struct pipe *p)
{
    wake_all(&p->rq);
    wake_all(&p->wq);
    poll_wake();
}

static s32 pipe_read(struct file *f, void *buf, u32 len)
{
    struct pipe *p = f->priv;
    u8 *out = buf;
    u32 n = 0;

    if (len == 0) {
        return 0;
    }
    for (;;) {
        if (p->count > 0) {
            while (n < len && p->count > 0) {
                out[n++] = p->buf[p->head];
                p->head = (p->head + 1) % PIPE_SIZE;
                p->count--;
            }
            wake_everyone(p);
            return (s32)n;
        }
        if (p->writers == 0) {
            return 0;                   /* end of file */
        }
        if (f->flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        sleep_on(&p->rq);
    }
}

static s32 pipe_write(struct file *f, const void *buf, u32 len)
{
    struct pipe *p = f->priv;
    const u8 *in = buf;
    u32 done = 0;

    while (done < len) {
        u32 room, want;

        if (p->readers == 0) {
            /* Nobody will ever read it. The signal is the usual way a
             * program finds out; EPIPE is for one that ignores it. */
            signal_send(current, SIGPIPE);
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
            if (f->flags & O_NONBLOCK) {
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

static int pipe_close(struct file *f)
{
    struct pipe *p = f->priv;

    if ((f->flags & O_ACCMODE) == O_WRONLY) {
        p->writers--;
    } else {
        p->readers--;
    }
    /* A reader waiting on the last writer, or a writer on the last
     * reader, has to find out now rather than sleep for ever. */
    wake_everyone(p);
    if (p->readers == 0 && p->writers == 0) {
        pmm_free((u32)p->buf);
        p->used = 0;
    }
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
};

int pipe_create(int fds[2], int flags)
{
    struct pipe *p = 0;
    int i, r, w;

    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) {
        return -EINVAL;
    }
    for (i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].used) {
            p = &pipes[i];
            break;
        }
    }
    if (!p) {
        return -ENFILE;
    }
    memset(p, 0, sizeof(*p));
    p->buf = (u8 *)pmm_alloc();
    if (!p->buf) {
        return -ENOMEM;
    }
    p->used = 1;
    p->readers = 1;
    p->writers = 1;

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
