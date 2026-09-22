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
 * sys/socket.h - the socket API, for Sage040.
 *
 * picolibc has no network layer; this is one, over the Linux/m68k system
 * calls the kernel implements. Every number here is Linux's, because the
 * values go to the kernel as they stand -- including SOCK_NONBLOCK and
 * SOCK_CLOEXEC, which are Linux's O_NONBLOCK and O_CLOEXEC, NOT
 * picolibc's own (picolibc translates open()'s flags; nothing translates
 * socket()'s).
 */
#ifndef _SYS_SOCKET_H_
#define _SYS_SOCKET_H_

#include <sys/types.h>
#include <sys/uio.h>

typedef unsigned int   socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        __ss_pad[126];
} __attribute__((aligned(4)));

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    size_t        msg_iovlen;
    void         *msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
};

struct linger {
    int l_onoff;
    int l_linger;
};

#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2
#define AF_INET6        10              /* for compiling; refused */
#define PF_UNSPEC       AF_UNSPEC
#define PF_UNIX         AF_UNIX
#define PF_LOCAL        AF_LOCAL
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_NONBLOCK   0x800           /* Linux's O_NONBLOCK */
#define SOCK_CLOEXEC    0x80000         /* Linux's O_CLOEXEC  */

#define SOL_SOCKET      1
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_LINGER       13
#define SO_REUSEPORT    15
#define SO_RCVLOWAT     18
#define SO_SNDLOWAT     19
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30

#define MSG_OOB         0x0001
#define MSG_PEEK        0x0002
#define MSG_DONTROUTE   0x0004
#define MSG_CTRUNC      0x0008
#define MSG_TRUNC       0x0020
#define MSG_DONTWAIT    0x0040
#define MSG_EOR         0x0080
#define MSG_WAITALL     0x0100
#define MSG_NOSIGNAL    0x4000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define SOMAXCONN       128

int     socket(int domain, int type, int protocol);
int     socketpair(int domain, int type, int protocol, int sv[2]);
int     bind(int fd, const struct sockaddr *addr, socklen_t len);
int     connect(int fd, const struct sockaddr *addr, socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr *addr, socklen_t *len);
int     accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
int     getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int     getpeername(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
int     getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int     setsockopt(int fd, int level, int name, const void *val,
                   socklen_t len);
int     shutdown(int fd, int how);

#endif
