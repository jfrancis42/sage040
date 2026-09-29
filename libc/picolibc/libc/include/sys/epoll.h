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

/*
 * sys/epoll.h -- picolibc 1.8.12 has none. Linux's. struct epoll_event
 * is 12 bytes on m68k, where a 64-bit member needs only 2-byte
 * alignment -- which is what the kernel reads. The calls are in
 * libos/linux/machine/m68k/events.c.
 */
#ifndef _SYS_EPOLL_H_
#define _SYS_EPOLL_H_

#include <sys/cdefs.h>
#include <stdint.h>
#include <signal.h>

_BEGIN_STD_C

#define EPOLL_CLOEXEC   0x80000

#define EPOLL_CTL_ADD   1
#define EPOLL_CTL_DEL   2
#define EPOLL_CTL_MOD   3

enum EPOLL_EVENTS {
    EPOLLIN        = 0x001,
    EPOLLPRI       = 0x002,
    EPOLLOUT       = 0x004,
    EPOLLERR       = 0x008,
    EPOLLHUP       = 0x010,
    EPOLLRDNORM    = 0x040,
    EPOLLRDBAND    = 0x080,
    EPOLLWRNORM    = 0x100,
    EPOLLWRBAND    = 0x200,
    EPOLLMSG       = 0x400,
    EPOLLRDHUP     = 0x2000,
    EPOLLEXCLUSIVE = 1u << 28,
    EPOLLWAKEUP    = 1u << 29,
    EPOLLONESHOT   = 1u << 30,
    EPOLLET        = 1u << 31
};
#define EPOLLIN        EPOLLIN
#define EPOLLPRI       EPOLLPRI
#define EPOLLOUT       EPOLLOUT
#define EPOLLERR       EPOLLERR
#define EPOLLHUP       EPOLLHUP
#define EPOLLRDNORM    EPOLLRDNORM
#define EPOLLRDBAND    EPOLLRDBAND
#define EPOLLWRNORM    EPOLLWRNORM
#define EPOLLWRBAND    EPOLLWRBAND
#define EPOLLMSG       EPOLLMSG
#define EPOLLRDHUP     EPOLLRDHUP
#define EPOLLEXCLUSIVE EPOLLEXCLUSIVE
#define EPOLLWAKEUP    EPOLLWAKEUP
#define EPOLLONESHOT   EPOLLONESHOT
#define EPOLLET        EPOLLET

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
};

int epoll_create(int __size);
int epoll_create1(int __flags);
int epoll_ctl(int __epfd, int __op, int __fd, struct epoll_event *__event);
int epoll_wait(int __epfd, struct epoll_event *__events, int __max,
               int __timeout);
int epoll_pwait(int __epfd, struct epoll_event *__events, int __max,
                int __timeout, const sigset_t *__mask);

_END_STD_C

#endif /* _SYS_EPOLL_H_ */
