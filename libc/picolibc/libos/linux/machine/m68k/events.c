/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright © 2026 Jeff Francis
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

/*
 * The event descriptors -- eventfd, timerfd, signalfd, epoll, inotify --
 * and ppoll/pselect, none of which picolibc's libos/linux (1.8.12) has.
 * Headers in libc/include/sys. Flags go to the kernel as they are: the
 * headers give them Linux's values.
 *
 * The work is in conversions. picolibc's time_t is 64 bits
 * and its struct timespec is { int64, long }, so time goes through the
 * kernel's *_time64 calls, whose timespec is { int64, int64 }. And its
 * SIGNALS ARE NUMBERED ITS OWN WAY -- SIGUSR1 is 30, where Linux's is 10
 * -- so every mask goes through _sigmask_to_linux, as sigprocmask's
 * does, and what a signalfd reads comes back through
 * _signal_from_linux (see sigfd_fix below). Its clocks are numbered
 * its own way too (timerfd_create), and m68k moves two poll bits
 * (ppoll).
 */

#include "../../local-linux.h"
#include "../../local-sigaction.h"
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/select.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

struct k_timespec64 {
    __int64_t tv_sec;
    __int64_t tv_nsec;
};

struct k_itimerspec64 {
    struct k_timespec64 it_interval;
    struct k_timespec64 it_value;
};

/* A picolibc mask as the kernel's, or NULL for none. */
static __kernel_sigset_t *
kmask(__kernel_sigset_t *k, const sigset_t *set)
{
    if (!set) {
        return NULL;
    }
    memset(k, 0, sizeof(*k));
    _sigmask_to_linux(k, set);
    return k;
}

static void
ts_out(struct k_timespec64 *k, const struct timespec *t)
{
    k->tv_sec = t->tv_sec;
    k->tv_nsec = t->tv_nsec;
}

static void
ts_in(struct timespec *t, const struct k_timespec64 *k)
{
    t->tv_sec = k->tv_sec;
    t->tv_nsec = (long)k->tv_nsec;
}

/* --- eventfd ---------------------------------------------------------- */

int
eventfd(unsigned int initval, int flags)
{
    return syscall(LINUX_SYS_eventfd2, initval, flags);
}

int
eventfd_read(int fd, eventfd_t *value)
{
    return read(fd, value, sizeof(*value)) == sizeof(*value) ? 0 : -1;
}

int
eventfd_write(int fd, eventfd_t value)
{
    return write(fd, &value, sizeof(value)) == sizeof(value) ? 0 : -1;
}

/* --- timerfd ---------------------------------------------------------- */

/*
 * picolibc numbers its clocks its own way (CLOCK_REALTIME is 1 and
 * CLOCK_MONOTONIC 4); Linux's are 0 and 1. As clock_gettime maps them.
 */
int
timerfd_create(int clockid, int flags)
{
    int kid;

    switch (clockid) {
    case CLOCK_REALTIME:  kid = 0; break;
    case CLOCK_MONOTONIC: kid = 1; break;
    case CLOCK_BOOTTIME:  kid = 7; break;
    default:              kid = -1; break;      /* EINVAL, from the kernel */
    }
    return syscall(LINUX_SYS_timerfd_create, kid, flags);
}

int
timerfd_settime(int fd, int flags, const struct itimerspec *new,
                struct itimerspec *old)
{
    struct k_itimerspec64 kn, ko;
    int r;

    ts_out(&kn.it_interval, &new->it_interval);
    ts_out(&kn.it_value, &new->it_value);
    r = syscall(LINUX_SYS_timerfd_settime64, fd, flags, &kn, old ? &ko : NULL);
    if (r == 0 && old) {
        ts_in(&old->it_interval, &ko.it_interval);
        ts_in(&old->it_value, &ko.it_value);
    }
    return r;
}

int
timerfd_gettime(int fd, struct itimerspec *cur)
{
    struct k_itimerspec64 k;
    int r = syscall(LINUX_SYS_timerfd_gettime64, fd, &k);

    if (r == 0) {
        ts_in(&cur->it_interval, &k.it_interval);
        ts_in(&cur->it_value, &k.it_value);
    }
    return r;
}

/* --- POSIX timers ----------------------------------------------------- */

/*
 * timer_create and the rest, over the kernel's (events.c there), in
 * this library's numbers: picolibc's clocks, its TIMER_ABSTIME (4,
 * Linux's 1), its SIGEV_ values (SIGEV_SIGNAL is 2, Linux's 0) and its
 * signal numbers all differ from the kernel's. SIGEV_THREAD -- a
 * function called on a new thread -- is a C library's to provide, and
 * this one does not.
 */
struct k_sigevent {
    int sigev_value;
    int sigev_signo;
    int sigev_notify;
    int sigev_tid;
    int _pad[12];
};

static int
kclock(clockid_t clockid)
{
    switch (clockid) {
    case CLOCK_REALTIME:  return 0;
    case CLOCK_MONOTONIC: return 1;
    case CLOCK_BOOTTIME:  return 7;
    default:              return -1;
    }
}

int
timer_create(clockid_t clockid, struct sigevent *evp, timer_t *timerid)
{
    struct k_sigevent k;
    int id = 0, r;

    memset(&k, 0, sizeof(k));
    if (evp) {
        switch (evp->sigev_notify) {
        case SIGEV_SIGNAL: k.sigev_notify = 0; break;
        case SIGEV_NONE:   k.sigev_notify = 1; break;
        default:
            errno = EINVAL;
            return -1;
        }
        k.sigev_signo = evp->sigev_notify == SIGEV_SIGNAL ?
                        _signal_to_linux(evp->sigev_signo) : 0;
        k.sigev_value = evp->sigev_value.sival_int;
    }
    r = syscall(LINUX_SYS_timer_create, kclock(clockid), evp ? &k : NULL, &id);
    if (r == 0) {
        *timerid = (timer_t)id;
    }
    return r;
}

int
timer_settime(timer_t timerid, int flags, const struct itimerspec *new,
              struct itimerspec *old)
{
    struct k_itimerspec64 kn, ko;
    int r;

    ts_out(&kn.it_interval, &new->it_interval);
    ts_out(&kn.it_value, &new->it_value);
    r = syscall(LINUX_SYS_timer_settime64, (int)timerid,
                (flags & TIMER_ABSTIME) ? 1 : 0, &kn, old ? &ko : NULL);
    if (r == 0 && old) {
        ts_in(&old->it_interval, &ko.it_interval);
        ts_in(&old->it_value, &ko.it_value);
    }
    return r;
}

int
timer_gettime(timer_t timerid, struct itimerspec *cur)
{
    struct k_itimerspec64 k;
    int r = syscall(LINUX_SYS_timer_gettime64, (int)timerid, &k);

    if (r == 0) {
        ts_in(&cur->it_interval, &k.it_interval);
        ts_in(&cur->it_value, &k.it_value);
    }
    return r;
}

int
timer_getoverrun(timer_t timerid)
{
    return syscall(LINUX_SYS_timer_getoverrun, (int)timerid);
}

int
timer_delete(timer_t timerid)
{
    return syscall(LINUX_SYS_timer_delete, (int)timerid);
}

/* --- signalfd --------------------------------------------------------- */

/*
 * A signalfd's records carry the KERNEL's signal numbers, which a
 * program compares with this library's SIGxxx and would never match.
 * read() is the only way to them, so read() asks: the descriptors
 * signalfd() made are marked here, and read.c (patch 43) hands what
 * it read on such a descriptor to __sage040_sigfd_fix, which rewrites
 * ssi_signo in place. close.c unmarks. The marks are memory, so fork
 * inherits them with the descriptors; a dup of one is not marked, and
 * neither is one inherited across exec -- those read Linux's numbers.
 */
#define SIGFD_BITS 1024

static unsigned long sigfds[SIGFD_BITS / 32];

int
__sage040_sigfd_is(int fd)
{
    return fd >= 0 && fd < SIGFD_BITS && (sigfds[fd / 32] >> (fd % 32)) & 1;
}

void
__sage040_sigfd_forget(int fd)
{
    if (fd >= 0 && fd < SIGFD_BITS) {
        sigfds[fd / 32] &= ~(1UL << (fd % 32));
    }
}

void
__sage040_sigfd_fix(void *buf, ssize_t n)
{
    struct signalfd_siginfo *si = buf;

    for (; n >= (ssize_t)sizeof(*si); n -= sizeof(*si), si++) {
        si->ssi_signo = (uint32_t)_signal_from_linux((int)si->ssi_signo);
    }
}

int
signalfd(int fd, const sigset_t *mask, int flags)
{
    __kernel_sigset_t k;
    int r = syscall(LINUX_SYS_signalfd4, fd, kmask(&k, mask), sizeof(k), flags);

    if (r >= 0 && r < SIGFD_BITS) {
        sigfds[r / 32] |= 1UL << (r % 32);
    }
    return r;
}

/* --- epoll ------------------------------------------------------------ */

_Static_assert(sizeof(struct epoll_event) == 12,
               "struct epoll_event must be Linux/m68k's 12 bytes");

int
epoll_create(int size)
{
    return syscall(LINUX_SYS_epoll_create, size);
}

int
epoll_create1(int flags)
{
    return syscall(LINUX_SYS_epoll_create1, flags);
}

int
epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    return syscall(LINUX_SYS_epoll_ctl, epfd, op, fd, event);
}

int
epoll_wait(int epfd, struct epoll_event *events, int max, int timeout)
{
    return syscall(LINUX_SYS_epoll_wait, epfd, events, max, timeout);
}

int
epoll_pwait(int epfd, struct epoll_event *events, int max, int timeout,
            const sigset_t *mask)
{
    __kernel_sigset_t k;

    return syscall(LINUX_SYS_epoll_pwait, epfd, events, max, timeout,
                   kmask(&k, mask), sizeof(k));
}

/* --- inotify ---------------------------------------------------------- */

int
inotify_init(void)
{
    return syscall(LINUX_SYS_inotify_init1, 0);
}

int
inotify_init1(int flags)
{
    return syscall(LINUX_SYS_inotify_init1, flags);
}

int
inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    return syscall(LINUX_SYS_inotify_add_watch, fd, path, mask);
}

int
inotify_rm_watch(int fd, int wd)
{
    return syscall(LINUX_SYS_inotify_rm_watch, fd, wd);
}

/* --- ppoll and pselect ------------------------------------------------ */

/*
 * The poll bits are Linux's except the two that m68k moves: its
 * POLLWRNORM is POLLOUT (4) and its POLLWRBAND is 0x100, where picolibc
 * has 0x100 and 0x200. As poll() maps them, and for every entry.
 */
static short
poll_to_linux(short e)
{
    short r = e & ~(POLLWRNORM | POLLWRBAND);

    if (e & POLLWRNORM)
        r |= POLLOUT;
    if (e & POLLWRBAND)
        r |= 0x100;
    return r;
}

static short
poll_from_linux(short e)
{
    short r = e & ~0x100;

    if (e & POLLOUT)
        r |= POLLWRNORM;
    if (e & 0x100)
        r |= POLLWRBAND;
    return r;
}

int
ppoll(struct pollfd *fds, nfds_t n, const struct timespec *ts,
      const sigset_t *mask)
{
    struct k_timespec64 kt;
    __kernel_sigset_t k;
    nfds_t i;
    int r;

    if (ts) {
        ts_out(&kt, ts);
    }
    for (i = 0; i < n; i++) {
        fds[i].events = poll_to_linux(fds[i].events);
    }
    r = syscall(LINUX_SYS_ppoll_time64, fds, n, ts ? &kt : NULL,
                kmask(&k, mask), sizeof(k));
    for (i = 0; i < n; i++) {
        /* Back as asked, except that POLLWRNORM, which is POLLOUT to
         * the kernel, comes back as POLLOUT -- as poll() leaves it. */
        fds[i].events = (short)(fds[i].events & ~0x100) |
                        ((fds[i].events & 0x100) ? POLLWRBAND : 0);
        fds[i].revents = r > 0 ? poll_from_linux(fds[i].revents) : 0;
    }
    return r;
}

int
pselect(int n, fd_set *rd, fd_set *wr, fd_set *ex,
        const struct timespec *ts, const sigset_t *mask)
{
    struct k_timespec64 kt;
    __kernel_sigset_t k;
    unsigned long data[2] = { (unsigned long)kmask(&k, mask), sizeof(k) };

    if (ts) {
        ts_out(&kt, ts);
    }
    return syscall(LINUX_SYS_pselect6_time64, n, rd, wr, ex, ts ? &kt : NULL,
                   mask ? data : NULL);
}
