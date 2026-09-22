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
 * socket.c - the socket calls, over Linux/m68k's system calls.
 *
 * Almost all of them pass straight through: the structures are Linux's
 * and so are the numbers. The exception is a timeout. SO_RCVTIMEO and
 * SO_SNDTIMEO take a struct timeval, and picolibc's has a 64-bit tv_sec
 * where Linux/m68k's has 32 -- so those two are converted on the way in
 * and out, or the kernel would read the high half of tv_sec as the
 * whole of it and a two-second timeout would be none at all.
 */
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

long syscall(long nr, ...);

#define NR_socket      356
#define NR_socketpair  357
#define NR_bind        358
#define NR_connect     359
#define NR_listen      360
#define NR_accept4     361
#define NR_getsockopt  362
#define NR_setsockopt  363
#define NR_getsockname 364
#define NR_getpeername 365
#define NR_sendto      366
#define NR_sendmsg     367
#define NR_recvfrom    368
#define NR_recvmsg     369
#define NR_shutdown    370

struct kernel_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

int socket(int domain, int type, int protocol)
{
    return (int)syscall(NR_socket, domain, type, protocol);
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    return (int)syscall(NR_socketpair, domain, type, protocol, sv);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return (int)syscall(NR_bind, fd, addr, len);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return (int)syscall(NR_connect, fd, addr, len);
}

int listen(int fd, int backlog)
{
    return (int)syscall(NR_listen, fd, backlog);
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    return (int)syscall(NR_accept4, fd, addr, len, flags);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return accept4(fd, addr, len, 0);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    return (int)syscall(NR_getsockname, fd, addr, len);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    return (int)syscall(NR_getpeername, fd, addr, len);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen)
{
    return (ssize_t)syscall(NR_sendto, fd, buf, len, flags, to, tolen);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen)
{
    return (ssize_t)syscall(NR_recvfrom, fd, buf, len, flags, from, fromlen);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return sendto(fd, buf, len, flags, 0, 0);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return recvfrom(fd, buf, len, flags, 0, 0);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    return (ssize_t)syscall(NR_sendmsg, fd, msg, flags);
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
{
    return (ssize_t)syscall(NR_recvmsg, fd, msg, flags);
}

static int is_timeout(int level, int name)
{
    return level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO);
}

int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    if (is_timeout(level, name)) {
        const struct timeval *tv = val;
        struct kernel_timeval k;

        if (!val || len < sizeof(struct timeval)) {
            errno = EINVAL;
            return -1;
        }
        if (tv->tv_sec > INT32_MAX) {
            k.tv_sec = INT32_MAX;       /* as good as for ever */
            k.tv_usec = 0;
        } else {
            k.tv_sec = (int32_t)tv->tv_sec;
            k.tv_usec = (int32_t)tv->tv_usec;
        }
        return (int)syscall(NR_setsockopt, fd, level, name, &k,
                            (socklen_t)sizeof(k));
    }
    return (int)syscall(NR_setsockopt, fd, level, name, val, len);
}

int getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    if (is_timeout(level, name)) {
        struct kernel_timeval k;
        socklen_t klen = sizeof(k);
        struct timeval *tv = val;
        int r;

        if (!val || !len || *len < sizeof(struct timeval)) {
            errno = EINVAL;
            return -1;
        }
        r = (int)syscall(NR_getsockopt, fd, level, name, &k, &klen);
        if (r < 0) {
            return r;
        }
        tv->tv_sec = k.tv_sec;
        tv->tv_usec = k.tv_usec;
        *len = sizeof(struct timeval);
        return 0;
    }
    return (int)syscall(NR_getsockopt, fd, level, name, val, len);
}

int shutdown(int fd, int how)
{
    return (int)syscall(NR_shutdown, fd, how);
}

/*
 * readv and writev, which the kernel does not have: a read or write per
 * piece. NOT atomic, as the kernel's would be -- another writer to the
 * same pipe can come between two pieces -- and a short transfer stops
 * the loop, as it must.
 */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t n = read(fd, iov[i].iov_base, iov[i].iov_len);

        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);

        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) {
            break;
        }
    }
    return total;
}
