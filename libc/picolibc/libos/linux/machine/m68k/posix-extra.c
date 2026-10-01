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

/*
 * pause, getpgid, usleep, select, flock, ftruncate, truncate, clock_getres
 * and clock_nanosleep are not in picolibc's libos/linux (1.8.12)
 * on any architecture. They are here, in the m68k backend, only so that
 * the release underneath stays unmodified; nothing in them is specific
 * to m68k, and they belong beside the other calls in libos/linux.
 */

#include "../../local-linux.h"
#include "../../local-time.h"
#include <sys/select.h>
#include <sys/file.h>
#include <time.h>
#include <asm/cachectl.h>

int
pause(void)
{
    return syscall(LINUX_SYS_pause);
}

/* Declared in <sys/unistd.h>, defined nowhere, and the kernel has had
 * the call all along: git's link was the first to ask. */
pid_t
getpgid(pid_t pid)
{
    return syscall(LINUX_SYS_getpgid, pid);
}

int
usleep(useconds_t usec)
{
    struct timespec ts;

    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (long)(usec % 1000000) * 1000;
    return nanosleep(&ts, NULL);
}

/*
 * Through _newselect, whose timeout is Linux/m68k's 32-bit timeval.
 * picolibc's time_t is 64 bits, so it is converted each way. The fd_set
 * needs nothing: both sides keep bit n in word n / 32.
 */
int
select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct {
        __int32_t tv_sec;
        __int32_t tv_usec;
    } ktv, *ktvp = NULL;
    int ret;

    if (tv) {
        ktv.tv_sec = (__int32_t)tv->tv_sec;
        ktv.tv_usec = (__int32_t)tv->tv_usec;
        ktvp = &ktv;
    }
    ret = syscall(LINUX_SYS__newselect, n, rd, wr, ex, ktvp);
    if (tv) {
        /* Linux writes back the time left, and so does this. */
        tv->tv_sec = ktv.tv_sec;
        tv->tv_usec = ktv.tv_usec;
    }
    return ret;
}

/* picolibc declares flock() in <sys/file.h>, with Linux's LOCK_ values. */
int
flock(int fd, int op)
{
    return syscall(LINUX_SYS_flock, fd, op);
}

/*
 * off_t is 64 bits and the calls here take 32: a length that does not
 * fit is refused rather than cut, since no file on a FAT volume can be
 * that long anyway.
 */
int
ftruncate(int fd, off_t len)
{
    if (len < 0 || len > 0x7fffffff) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_ftruncate, fd, (long)len);
}

int
truncate(const char *path, off_t len)
{
    if (len < 0 || len > 0x7fffffff) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_truncate, path, (long)len);
}

/*
 * Referenced by timespec_getres() in picolibc and defined nowhere, so a
 * static program that used it would not link and libc.so, which links
 * everything, did not. The clock ids map exactly as clock_gettime's do.
 */
int
clock_getres(clockid_t id, struct timespec *res)
{
    struct __kernel_timespec kts;
    int                      kid;
    int                      ret;

    if (id == CLOCK_MONOTONIC)
        kid = LINUX_CLOCK_MONOTONIC;
    else if (id == CLOCK_PROCESS_CPUTIME_ID)
        kid = LINUX_CLOCK_PROCESS_CPUTIME_ID;
    else if (id == CLOCK_REALTIME)
        kid = LINUX_CLOCK_REALTIME;
    else if (id == CLOCK_THREAD_CPUTIME_ID)
        kid = LINUX_CLOCK_THREAD_CPUTIME_ID;
    else {
        errno = EINVAL;
        return -1;
    }
    ret = syscall(LINUX_SYS_clock_getres, kid, &kts);
    if (ret < 0)
        return ret;
    if (res) {
        res->tv_sec = kts.tv_sec;
        res->tv_nsec = kts.tv_nsec;
    }
    return 0;
}

/*
 * cacheflush(2) -- m68k's, and only m68k's.
 *
 * A program that writes instructions into memory and then jumps to them
 * has to say so on this CPU, because the 68040's data and instruction
 * caches are separate and the store went into one while the fetch comes
 * from the other. libffi's closures are the caller here.
 *
 * Nothing to translate: the scopes and caches in <asm/cachectl.h> are
 * the kernel's own numbers, and the errno comes back as it does from
 * any other call. See kernel/cache.c for what the kernel does with it.
 */
int
cacheflush(void *addr, int scope, int cache, size_t len)
{
    return syscall(LINUX_SYS_cacheflush, addr, scope, cache, len);
}

/*
 * clock_nanosleep, over nanosleep and clock_gettime.
 *
 * <time.h> has always declared it and nothing defined it, so a program
 * that called it compiled, linked -- a shared object may leave symbols
 * undefined -- and failed only when loaded: Perl's Time::HiRes did,
 * with "undefined symbol: clock_nanosleep".
 *
 * The kernel's timers have a 10 ms tick and no per-clock sleep, so a
 * relative sleep is nanosleep whichever clock is named, and an absolute
 * one reads the named clock and sleeps the difference -- again, if a
 * signal did not end it early, until the clock says the time has come.
 * As POSIX has it, the result is an error number, not -1 and errno.
 */
int
clock_nanosleep(clockid_t id, int flags, const struct timespec *req,
                struct timespec *rem)
{
    struct timespec now, d;
    int saved = errno, err = 0;

    if (id != CLOCK_REALTIME && id != CLOCK_MONOTONIC)
        return EINVAL;
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
        return EINVAL;
    if (!(flags & TIMER_ABSTIME)) {
        if (nanosleep(req, rem) < 0)
            err = errno;
        errno = saved;
        return err;
    }
    for (;;) {
        if (clock_gettime(id, &now) < 0) {
            err = errno;
            break;
        }
        d.tv_sec = req->tv_sec - now.tv_sec;
        d.tv_nsec = req->tv_nsec - now.tv_nsec;
        if (d.tv_nsec < 0) {
            d.tv_nsec += 1000000000L;
            d.tv_sec--;
        }
        if (d.tv_sec < 0 || (d.tv_sec == 0 && d.tv_nsec == 0))
            break;
        if (nanosleep(&d, NULL) < 0) {
            err = errno;        /* EINTR; rem is not written for ABSTIME */
            break;
        }
    }
    errno = saved;
    return err;
}
