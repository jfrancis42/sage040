/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * poll.c - waiting on several descriptors at once.
 *
 * poll() and select() are one loop: ask every descriptor whether it is
 * ready, return if any is, otherwise sleep and ask again. What makes it
 * cheap is who does the waking. The terminal wakes poll_wait from the
 * tick the moment input arrives, so a program waiting on its keyboard
 * sleeps until there is a key. The sleep also has a timeout of its own
 * -- a tenth of a second, or a fiftieth when a socket is being watched
 * -- because the network does its protocol work only when somebody
 * asks, and a wakeup missed for any reason should cost a short delay
 * rather than a hang. The same trade tty.c makes for a blocking read.
 *
 * A descriptor says how ready it is through file_ops->poll, and when it
 * has none, through FIONREAD; see file_ready().
 */
#include "poll.h"
#include "vfs.h"
#include "dev.h"
#include "task.h"
#include "wait.h"
#include "timer.h"
#include "signal.h"
#include "errno.h"
#include "string.h"

static struct waitq poll_wait;

void poll_wake(void)
{
    wake_all(&poll_wait);
}

/*
 * THE EARLIEST DEADLINE among the descriptors a scan just looked at: a
 * timerfd (events.c) says when it next expires, and the sleep is cut to
 * then, so a timer wakes its poller on time rather than at the end of
 * the slice. Reset at the start of each scan; 0 means none.
 */
static u32 next_deadline;

void poll_deadline(u32 at)
{
    if (!next_deadline || (s32)(at - next_deadline) < 0) {
        next_deadline = at ? at : 1;
    }
}

void poll_scan_begin(void)
{
    next_deadline = 0;
}

/* Sleep until woken or `slice` ms, less if a deadline comes first. */
void poll_sleep(u32 slice)
{
    if (next_deadline) {
        s32 d = (s32)(next_deadline - timer_jiffies());

        if (d <= 0) {
            return;
        }
        if ((u32)d * (1000 / HZ) < slice) {
            slice = (u32)d * (1000 / HZ);
        }
    }
    sleep_on_timeout(&poll_wait, slice);
}

/* POLL* bits for one descriptor, now. POLLNVAL if it is not open. */
int poll_file(struct file *f, int *is_socketish)
{
    u32 n = 0;

    if (!f || !f->ops) {
        return POLLNVAL;
    }
    if (f->ops->poll) {
        *is_socketish = 1;      /* has its own idea; ask it more often */
        return f->ops->poll(f);
    }
    /*
     * Anything that can say how many bytes are waiting -- the terminal,
     * a serial port, the keyboard -- is readable exactly when that is
     * not zero. Anything that cannot is a file, and a read of a file
     * never waits, so it is always ready: at the end of it, a read
     * returns 0 at once, which is "ready" in poll's sense.
     */
    if (f->ops->ioctl && f->ops->ioctl(f, FIONREAD, (u32)&n) == 0) {
        return (n ? POLLIN : 0) | (f->ops->write ? POLLOUT : 0);
    }
    return POLLIN | POLLOUT;
}

static int file_ready(int fd, int *is_socketish)
{
    return poll_file(fd_get(fd), is_socketish);
}

int poll_fd(int fd)
{
    int ignored = 0;

    return file_ready(fd, &ignored);
}

s32 poll_ms_to_ticks(s32 ms)
{
    /* Rounded up: a wait of 1 ms must not be a wait of nothing. */
    return (ms * HZ + 999) / 1000;
}

int poll_files(struct pollfd *fds, u32 n, s32 timeout_ms, s32 *left_ms)
{
    u32 start = timer_jiffies();
    s32 limit = timeout_ms > 0 ? poll_ms_to_ticks(timeout_ms) : 0;
    u32 i;

    for (;;) {
        int count = 0, socketish = 0;
        s32 slice;

        poll_scan_begin();
        for (i = 0; i < n; i++) {
            int r;

            if (fds[i].fd < 0) {
                fds[i].revents = 0;     /* POSIX: ignored, not an error */
                continue;
            }
            r = file_ready(fds[i].fd, &socketish);
            /* Errors and hang-ups are reported whether asked for or not. */
            fds[i].revents = (short)(r & (fds[i].events |
                                          POLLERR | POLLHUP | POLLNVAL));
            if (fds[i].revents) {
                count++;
            }
        }

        if (left_ms && timeout_ms > 0) {
            s32 used = (s32)(timer_jiffies() - start) * (1000 / HZ);

            *left_ms = used < timeout_ms ? timeout_ms - used : 0;
        }
        if (count || timeout_ms == 0) {
            return count;
        }
        if (signal_pending(current)) {
            /* -EINTR after a handler, a restart if none ran: signal.c. */
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

int poll_select(u32 nfds, u32 *in, u32 *out, u32 *ex, s32 timeout_ms,
                s32 *left_ms)
{
    struct pollfd fds[OPEN_MAX];
    u32 fd, words = (nfds + 31) / 32, n = 0, i;
    int r, count;

    for (fd = 0; fd < nfds; fd++) {
        u32 bit = 1UL << (fd % 32);
        short ev = 0;

        if (in && (in[fd / 32] & bit))  ev |= POLLIN;
        if (out && (out[fd / 32] & bit)) ev |= POLLOUT;
        if (ex && (ex[fd / 32] & bit))  ev |= POLLPRI;
        if (!ev) {
            continue;
        }
        /* select() refuses a set bit for a descriptor that is not open,
         * where poll() would report POLLNVAL for it. */
        if (fd >= OPEN_MAX || !fd_get((int)fd)) {
            return -EBADF;
        }
        fds[n].fd = (int)fd;
        fds[n].events = ev;
        fds[n].revents = 0;
        n++;
    }

    r = poll_files(fds, n, timeout_ms, left_ms);
    if (r < 0) {
        return r;
    }

    for (i = 0; i < words; i++) {
        if (in)  in[i] = 0;
        if (out) out[i] = 0;
        if (ex)  ex[i] = 0;
    }
    count = 0;
    for (i = 0; i < n; i++) {
        u32 w = (u32)fds[i].fd / 32, bit = 1UL << (fds[i].fd % 32);
        short rv = fds[i].revents;

        /* select's meaning: an error or hang-up makes a descriptor
         * readable and writable, because a read or write will not wait. */
        if (in && (fds[i].events & POLLIN) && (rv & (POLLIN | POLLHUP | POLLERR))) {
            in[w] |= bit;
            count++;
        }
        if (out && (fds[i].events & POLLOUT) && (rv & (POLLOUT | POLLERR))) {
            out[w] |= bit;
            count++;
        }
        if (ex && (fds[i].events & POLLPRI) && (rv & POLLPRI)) {
            ex[w] |= bit;
            count++;
        }
    }
    return count;
}
