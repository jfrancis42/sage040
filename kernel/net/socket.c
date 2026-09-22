/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * socket.c - a connection that a program can hold.
 *
 * A SOCKET IS A FILE DESCRIPTOR, with `struct file_ops` like a file on
 * the disk or a device under /dev. That is not a convenience: it is the
 * reason read(), write() and close() work on one without knowing what
 * it is, and it is why a program can be pointed at a socket instead of
 * a file and not notice. Unix has done it this way since 4.2BSD and
 * there is nothing to improve on.
 *
 * It is also what keeps the TCP underneath replaceable. A program calls
 * socket(), connect() and read(); which implementation answers is not
 * its business.
 *
 * THE INTERFACE IS LINUX'S: struct sockaddr with a length, the flags
 * arguments, accept4, getsockname and getpeername, setsockopt and
 * getsockopt. The system call layer copies addresses in and out; every
 * function here takes kernel pointers and a struct file.
 *
 * WAITING is one function, sock_wait(): it drives the protocol while it
 * waits (netd does too, fifty times a second, whether or not anybody is
 * waiting), and it is where O_NONBLOCK, MSG_DONTWAIT, SO_RCVTIMEO,
 * SO_SNDTIMEO and signals are all decided. A blocking call waits for as
 * long as it takes, as it does everywhere else; it used to give up
 * after thirty seconds with ETIMEDOUT, which no POSIX read does.
 */
#include "net.h"
#include "tcp.h"
#include "vfs.h"
#include "dev.h"
#include "timer.h"
#include "task.h"
#include "signal.h"
#include "poll.h"
#include "pmm.h"
#include "errno.h"
#include "string.h"

#define SOCK_MAX        32
#define CONNECT_MS      75000   /* BSD's connect timeout */

/* Received datagrams, queued: two pages of them per UDP socket. */
#define DGRAM_MAX       (NET_MTU - ETH_HDR_LEN - 20 - 8)    /* 1472 */
#define DGRAM_SLOTS     4
#define DGRAM_PAGES     2

struct dgram {
    u32   len;
    ip4_t from;
    u16   port;
    u8    data[DGRAM_MAX];
};

struct socket {
    int   used;
    int   type;                 /* SOCK_STREAM or SOCK_DGRAM */
    struct tcpcb *tcp;

    u16   port;                 /* bound local port, 0 if none */
    int   bound;
    ip4_t bound_ip;             /* INADDR_ANY, 127.x, or ours */

    /* A datagram socket's connect() records where write() sends. */
    ip4_t peer_ip;
    u16   peer_port;

    struct dgram *q;            /* DGRAM_SLOTS of them */
    u32   q_head, q_count;

    /* Options. */
    int   reuseaddr, keepalive, broadcast;
    u32   rcvtimeo_ms, sndtimeo_ms;
    int   so_error;
    int   connecting;           /* a non-blocking connect in progress */
    int   shut_rd, shut_wr;
};

static struct socket sockets[SOCK_MAX];

static const struct file_ops sock_ops;

static struct socket *sock_alloc(void)
{
    int i;

    for (i = 0; i < SOCK_MAX; i++) {
        if (!sockets[i].used) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].used = 1;
            return &sockets[i];
        }
    }
    return 0;
}

int sock_is(struct file *f)
{
    return f && f->ops == &sock_ops;
}

int sock_dgram(struct file *f)
{
    return ((struct socket *)f->priv)->type == SOCK_DGRAM;
}

static int nonblocking(struct file *f, int flags)
{
    return (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
}

/* --- waiting ---------------------------------------------------------- */

/*
 * 0 once `ready` says so; -EAGAIN at once for a non-blocking call, or
 * when `timeout_ms` (0: none) runs out; -EINTR for a signal -- which the
 * task 5 rules turn into a restart or EINTR as the handler asks.
 */
static int sock_wait(struct socket *s, int nonblock, u32 timeout_ms,
                     int (*ready)(struct socket *))
{
    u32 deadline = timer_jiffies() + (timeout_ms * HZ + 999) / 1000;

    for (;;) {
        net_poll();
        tcp_timer();
        if (ready(s)) {
            return 0;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        if (timeout_ms && (s32)(timer_jiffies() - deadline) >= 0) {
            return -EAGAIN;     /* SO_RCVTIMEO's answer, as Linux's */
        }
        net_sleep(20);
    }
}

static int can_read(struct socket *s)
{
    if (s->shut_rd) {
        return 1;
    }
    if (s->type == SOCK_DGRAM) {
        return s->q_count > 0;
    }
    return !s->tcp || (tcp_poll(s->tcp) & (POLLIN | POLLHUP | POLLERR));
}

static int can_write(struct socket *s)
{
    return !s->tcp || (tcp_poll(s->tcp) & (POLLOUT | POLLERR | POLLHUP));
}

static int can_accept(struct socket *s)
{
    return (tcp_poll(s->tcp) & POLLIN) != 0;
}

static int connect_done(struct socket *s)
{
    return s->tcp->state != TCP_SYN_SENT;
}

/* Why a connection attempt failed, from what the connection says. */
static int connect_error(struct tcpcb *t)
{
    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        return 0;
    }
    if (t->timed_out) {
        return -ETIMEDOUT;
    }
    return -ECONNREFUSED;
}

/* --- datagrams -------------------------------------------------------- */

static void dgram_arrived(void *arg, ip4_t from, u16 sport,
                          const void *data, u32 len)
{
    struct socket *s = arg;
    struct dgram *d;

    if (s->q_count == DGRAM_SLOTS || s->shut_rd) {
        return;                 /* full: dropped, as UDP is allowed to */
    }
    d = &s->q[(s->q_head + s->q_count) % DGRAM_SLOTS];
    if (len > DGRAM_MAX) {
        len = DGRAM_MAX;
    }
    memcpy(d->data, data, len);
    d->len = len;
    d->from = from;
    d->port = sport;
    s->q_count++;
    poll_wake();
}

static u16 pick_udp_port(void)
{
    static u16 next = 32768;
    int tries;

    for (tries = 0; tries < 28232; tries++) {
        u16 p = next++;

        if (next == 0 || next < 32768) {
            next = 32768;
        }
        if (!udp_port_in_use(p)) {
            return p;
        }
    }
    return 0;
}

static int dgram_autobind(struct socket *s)
{
    if (s->bound) {
        return 0;
    }
    s->port = pick_udp_port();
    if (!s->port || udp_bind(s->port, dgram_arrived, s) < 0) {
        return -EADDRINUSE;
    }
    s->bound = 1;
    return 0;
}

/* --- the file operations ---------------------------------------------- */

static s32 sock_read(struct file *f, void *buf, u32 len)
{
    return sock_recv(f, buf, len, 0, 0, 0);
}

static s32 sock_write(struct file *f, const void *buf, u32 len)
{
    return sock_send(f, buf, len, 0, 0);
}

static int sock_ioctl(struct file *f, u32 request, u32 arg)
{
    struct socket *s = f->priv;

    if (request == FIONREAD) {
        u32 n = 0;

        net_poll();
        if (s->type == SOCK_DGRAM) {
            n = s->q_count ? s->q[s->q_head].len : 0;
        } else if (s->tcp) {
            n = tcp_available(s->tcp);
        }
        if (arg) {
            *(u32 *)arg = n;
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

/*
 * Closing does not wait. The connection is handed to TCP to finish on
 * its own -- the FIN, the peer's, TIME_WAIT -- and netd runs its timers
 * until it is done. It used to linger here for half a second and then
 * free the connection whatever state it was in, which was the honest
 * best when nothing could run the connection after its descriptor went.
 */
static int sock_close(struct file *f)
{
    struct socket *s = f->priv;

    if (s->type == SOCK_DGRAM) {
        if (s->bound) {
            udp_unbind(s->port);
        }
        if (s->q) {
            pmm_free_pages((u32)s->q, DGRAM_PAGES);
        }
    } else if (s->tcp) {
        tcp_release(s->tcp);
    }
    s->used = 0;
    return 0;
}

static int sock_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFSOCK;
    st->st_size = 0;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static int sock_poll(struct file *f)
{
    struct socket *s = f->priv;

    net_poll();
    if (s->type == SOCK_DGRAM) {
        return (can_read(s) ? POLLIN : 0) | POLLOUT;
    }
    if (!s->tcp) {
        return POLLOUT;
    }
    return tcp_poll(s->tcp) | (s->shut_rd ? POLLIN : 0);
}

/*
 * A socket is S_IFSOCK. It said S_IFCHR once, back when isatty() was
 * built on S_ISCHR -- which made every socket a terminal.
 */
static const struct file_ops sock_ops = {
    sock_read,
    sock_write,
    0,                          /* a socket is not seekable */
    sock_ioctl,
    sock_close,
    sock_fstat,
    sock_poll,
    0,                          /* truncate: nothing to truncate */
};

/* --- the calls -------------------------------------------------------- */

int sock_create(int domain, int type, int protocol)
{
    struct socket *s;
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int fd;

    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain == AF_UNIX) {
        return -EAFNOSUPPORT;   /* only socketpair(): see uapi.h */
    }
    if (domain != AF_INET) {
        return -EAFNOSUPPORT;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        return -EPROTONOSUPPORT;
    }
    if (protocol && protocol != (type == SOCK_STREAM ? IPPROTO_TCP
                                                      : IPPROTO_UDP)) {
        return -EPROTONOSUPPORT;
    }

    s = sock_alloc();
    if (!s) {
        return -ENFILE;
    }
    s->type = type;
    if (type == SOCK_STREAM) {
        s->tcp = tcp_alloc();
        if (!s->tcp) {
            s->used = 0;
            return -ENOBUFS;
        }
    } else {
        s->q = (struct dgram *)pmm_alloc_pages(DGRAM_PAGES);
        if (!s->q) {
            s->used = 0;
            return -ENOMEM;
        }
    }

    fd = fd_install(&sock_ops, s, O_RDWR | flags);
    if (fd < 0) {
        if (s->tcp) {
            tcp_free(s->tcp);
        }
        if (s->q) {
            pmm_free_pages((u32)s->q, DGRAM_PAGES);
        }
        s->used = 0;
    }
    return fd;
}

static int addr_is_local(ip4_t a)
{
    struct netif *n = net_if();

    return a == INADDR_ANY || IP4_IS_LOOPBACK(a) || (n->ip && a == n->ip);
}

int sock_bind(struct file *f, const struct sockaddr_in *sa)
{
    struct socket *s = f->priv;
    u16 port = sa->sin_port;

    if (sa->sin_family != AF_INET) {
        return -EAFNOSUPPORT;
    }
    if (s->bound) {
        return -EINVAL;
    }
    if (!addr_is_local(sa->sin_addr.s_addr)) {
        return -EADDRNOTAVAIL;
    }

    if (s->type == SOCK_DGRAM) {
        if (!port) {
            port = pick_udp_port();
        }
        if (!port || udp_bind(port, dgram_arrived, s) < 0) {
            return -EADDRINUSE;
        }
    } else {
        if (!port) {
            port = tcp_pick_port();
            if (!port) {
                return -EADDRINUSE;
            }
        } else if (tcp_port_in_use(port) && !s->reuseaddr) {
            /* In use -- by a listener, a live connection, or one in
             * TIME_WAIT. SO_REUSEADDR is what lets a restarted server
             * take its port back from the last one's TIME_WAIT. */
            return -EADDRINUSE;
        }
        s->tcp->local_port = port;
    }
    s->port = port;
    s->bound_ip = sa->sin_addr.s_addr;
    s->bound = 1;
    return 0;
}

int sock_connect(struct file *f, const struct sockaddr_in *sa)
{
    struct socket *s = f->priv;
    int err;

    if (sa->sin_family != AF_INET) {
        return -EAFNOSUPPORT;
    }

    if (s->type == SOCK_DGRAM) {
        /* No handshake: it only records where write() should send. */
        err = dgram_autobind(s);
        if (err < 0) {
            return err;
        }
        s->peer_ip = sa->sin_addr.s_addr;
        s->peer_port = sa->sin_port;
        return 0;
    }

    if (s->tcp->state == TCP_SYN_SENT) {
        return -EALREADY;
    }
    if (s->tcp->state != TCP_CLOSED) {
        return -EISCONN;
    }
    err = tcp_connect(s->tcp, sa->sin_addr.s_addr, sa->sin_port);
    if (err < 0) {
        return err;
    }
    s->bound = 1;
    s->port = s->tcp->local_port;

    /* Non-blocking: it goes on without the caller, which finds out by
     * poll() for POLLOUT and then SO_ERROR. */
    if (f->flags & O_NONBLOCK) {
        s->connecting = 1;
        return -EINPROGRESS;
    }

    err = sock_wait(s, 0, CONNECT_MS, connect_done);
    if (err == -EINTR) {
        s->connecting = 1;      /* carries on; SO_ERROR will say */
        return -EINTR;
    }
    if (err < 0) {
        return -ETIMEDOUT;
    }
    return connect_error(s->tcp);
}

int sock_listen(struct file *f, int backlog)
{
    struct socket *s = f->priv;

    (void)backlog;
    if (s->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (s->tcp->state == TCP_LISTEN) {
        return 0;
    }
    if (!s->bound) {
        /* Linux binds an unbound listener to a free port. */
        s->port = tcp_pick_port();
        if (!s->port) {
            return -EADDRINUSE;
        }
        s->tcp->local_port = s->port;
        s->bound = 1;
    }
    return tcp_listen(s->tcp, s->port);
}

int sock_accept(struct file *f, struct sockaddr_in *peer, int flags)
{
    struct socket *s = f->priv;
    struct socket *c;
    struct tcpcb *t;
    int fd, err;

    if (s->type != SOCK_STREAM || s->tcp->state != TCP_LISTEN) {
        return -EINVAL;
    }
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        return -EINVAL;
    }
    err = sock_wait(s, nonblocking(f, 0), s->rcvtimeo_ms, can_accept);
    if (err < 0) {
        return err;
    }
    t = tcp_accept(s->tcp);
    if (!t) {
        return -EAGAIN;
    }

    c = sock_alloc();
    if (!c) {
        tcp_release(t);
        return -ENFILE;
    }
    c->type = SOCK_STREAM;
    c->tcp = t;
    c->bound = 1;
    c->port = t->local_port;
    c->bound_ip = t->local_ip;

    fd = fd_install(&sock_ops, c, O_RDWR | flags);
    if (fd < 0) {
        tcp_release(t);
        c->used = 0;
        return fd;
    }
    if (peer) {
        memset(peer, 0, sizeof(*peer));
        peer->sin_family = AF_INET;
        peer->sin_addr.s_addr = t->remote_ip;
        peer->sin_port = t->remote_port;
    }
    return fd;
}

s32 sock_send(struct file *f, const void *buf, u32 len, int flags,
              const struct sockaddr_in *to)
{
    struct socket *s = f->priv;
    const u8 *p = buf;
    u32 done = 0;
    int err;

    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) {
        return -EOPNOTSUPP;
    }
    if (s->shut_wr) {
        if (!(flags & MSG_NOSIGNAL)) {
            signal_send(current, SIGPIPE);
        }
        return -EPIPE;
    }

    if (s->type == SOCK_DGRAM) {
        ip4_t dst = to ? to->sin_addr.s_addr : s->peer_ip;
        u16 dport = to ? to->sin_port : s->peer_port;

        if (to && to->sin_family != AF_INET) {
            return -EAFNOSUPPORT;
        }
        if (!to && !s->peer_ip && !s->peer_port) {
            return -EDESTADDRREQ;
        }
        if (len > DGRAM_MAX) {
            return -EMSGSIZE;   /* whole or not at all: it is a datagram */
        }
        err = dgram_autobind(s);
        if (err < 0) {
            return err;
        }
        err = udp_output(dst, dport, s->port, buf, len);
        return err < 0 ? err : (s32)len;
    }

    if (to) {
        return -EISCONN;        /* a stream has only one peer */
    }
    if (!s->tcp || s->tcp->state == TCP_LISTEN ||
        (s->tcp->state == TCP_CLOSED && !s->tcp->reset)) {
        return -ENOTCONN;
    }

    /*
     * A short write is legal, but a write() that returns zero having
     * been given data looks to a caller like a closed stream. So this
     * keeps going until at least something has gone, and returns short
     * only after that.
     */
    while (done < len) {
        s32 n = tcp_send(s->tcp, p + done, len - done);

        if (n < 0) {
            if (done > 0) {
                break;
            }
            if (n == -EPIPE || n == -ECONNRESET) {
                /* The stream is gone; the signal is how a program that
                 * is not checking finds out, as with a pipe. */
                if (!(flags & MSG_NOSIGNAL)) {
                    signal_send(current, SIGPIPE);
                }
                return -EPIPE;
            }
            return n;
        }
        done += (u32)n;
        if (n == 0) {
            err = sock_wait(s, nonblocking(f, flags), s->sndtimeo_ms,
                            can_write);
            if (err < 0) {
                return done > 0 ? (s32)done : err;
            }
        }
    }
    return (s32)done;
}

s32 sock_recv(struct file *f, void *buf, u32 len, int flags,
              struct sockaddr_in *from, int *truncated)
{
    struct socket *s = f->priv;
    int err;

    if (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_WAITALL | MSG_TRUNC)) {
        return -EOPNOTSUPP;
    }
    if (truncated) {
        *truncated = 0;
    }
    if (s->shut_rd) {
        return 0;
    }

    if (s->type == SOCK_DGRAM) {
        struct dgram *d;
        u32 n;

        if (!s->bound) {
            /* Nothing can arrive for a socket with no port. */
            err = dgram_autobind(s);
            if (err < 0) {
                return err;
            }
        }
        err = sock_wait(s, nonblocking(f, flags), s->rcvtimeo_ms, can_read);
        if (err < 0) {
            return err;
        }
        if (s->shut_rd) {
            return 0;
        }
        d = &s->q[s->q_head];
        n = len < d->len ? len : d->len;
        memcpy(buf, d->data, n);
        if (truncated && n < d->len) {
            *truncated = 1;
        }
        if (from) {
            memset(from, 0, sizeof(*from));
            from->sin_family = AF_INET;
            from->sin_addr.s_addr = d->from;
            from->sin_port = d->port;
        }
        /* MSG_TRUNC asks for the datagram's real length. */
        err = (flags & MSG_TRUNC) ? (s32)d->len : (s32)n;
        if (!(flags & MSG_PEEK)) {
            s->q_head = (s->q_head + 1) % DGRAM_SLOTS;
            s->q_count--;
        }
        return err;
    }

    if (!s->tcp || s->tcp->state == TCP_LISTEN) {
        return -ENOTCONN;
    }
    {
        u32 done = 0;

        for (;;) {
            s32 n;

            err = sock_wait(s, nonblocking(f, flags) && !done,
                            s->rcvtimeo_ms, can_read);
            if (err < 0) {
                return done > 0 ? (s32)done : err;
            }
            if (s->shut_rd) {
                return (s32)done;
            }
            n = (flags & MSG_PEEK) ? tcp_peek(s->tcp, (u8 *)buf + done,
                                              len - done)
                                   : tcp_recv(s->tcp, (u8 *)buf + done,
                                              len - done);
            if (n == -EAGAIN) {
                continue;
            }
            if (n <= 0) {
                return done > 0 ? (s32)done : n;
            }
            done += (u32)n;
            /* MSG_WAITALL: keep going until it is all here, or the
             * stream ends, or a signal says stop. */
            if (!(flags & MSG_WAITALL) || (flags & MSG_PEEK) ||
                done == len) {
                return (s32)done;
            }
        }
    }
}

int sock_shutdown(struct file *f, int how)
{
    struct socket *s = f->priv;

    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
        return -EINVAL;
    }
    if (s->type == SOCK_STREAM &&
        (!s->tcp || s->tcp->state == TCP_CLOSED ||
         s->tcp->state == TCP_LISTEN || s->tcp->state == TCP_SYN_SENT)) {
        return -ENOTCONN;
    }
    if (how != SHUT_WR) {
        s->shut_rd = 1;
        poll_wake();
    }
    if (how != SHUT_RD && !s->shut_wr) {
        s->shut_wr = 1;
        /* The FIN. Only a connection that has one to send: tcp_close
         * frees a connection outright in the states that do not. */
        if (s->type == SOCK_STREAM) {
            tcp_close(s->tcp);
        }
    }
    return 0;
}

int sock_name(struct file *f, struct sockaddr_in *sa, int peer)
{
    struct socket *s = f->priv;

    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    if (peer) {
        if (s->type == SOCK_DGRAM) {
            if (!s->peer_ip && !s->peer_port) {
                return -ENOTCONN;
            }
            sa->sin_addr.s_addr = s->peer_ip;
            sa->sin_port = s->peer_port;
            return 0;
        }
        if (!s->tcp || (s->tcp->state != TCP_ESTABLISHED &&
                        s->tcp->state != TCP_CLOSE_WAIT &&
                        s->tcp->state != TCP_FIN_WAIT_1 &&
                        s->tcp->state != TCP_FIN_WAIT_2)) {
            return -ENOTCONN;
        }
        sa->sin_addr.s_addr = s->tcp->remote_ip;
        sa->sin_port = s->tcp->remote_port;
        return 0;
    }
    if (s->type == SOCK_STREAM && s->tcp &&
        s->tcp->state != TCP_CLOSED && s->tcp->state != TCP_LISTEN) {
        sa->sin_addr.s_addr = s->tcp->local_ip;
        sa->sin_port = s->tcp->local_port;
    } else {
        sa->sin_addr.s_addr = s->bound_ip;
        sa->sin_port = s->port;
    }
    return 0;
}

static u32 tv_ms(const struct timeval *tv)
{
    return (u32)tv->tv_sec * 1000 + ((u32)tv->tv_usec + 999) / 1000;
}

int sock_setopt(struct file *f, int level, int name, const void *val,
                u32 len)
{
    struct socket *s = f->priv;
    int v = len >= sizeof(int) ? *(const int *)val : 0;

    if (level == IPPROTO_TCP && name == TCP_NODELAY) {
        return len >= sizeof(int) ? 0 : -EINVAL;    /* always on */
    }
    if (level != SOL_SOCKET) {
        return -ENOPROTOOPT;
    }
    switch (name) {
    case SO_RCVTIMEO:
    case SO_SNDTIMEO: {
        const struct timeval *tv = val;

        if (len < sizeof(*tv) || tv->tv_sec < 0 || tv->tv_usec < 0 ||
            tv->tv_usec >= 1000000) {
            return -EINVAL;
        }
        if (name == SO_RCVTIMEO) {
            s->rcvtimeo_ms = tv_ms(tv);
        } else {
            s->sndtimeo_ms = tv_ms(tv);
        }
        return 0;
    }
    case SO_REUSEADDR:
    case SO_KEEPALIVE:
    case SO_BROADCAST:
    case SO_SNDBUF:
    case SO_RCVBUF:
        if (len < sizeof(int)) {
            return -EINVAL;
        }
        if (name == SO_REUSEADDR) {
            s->reuseaddr = v != 0;
        } else if (name == SO_KEEPALIVE) {
            s->keepalive = v != 0;  /* recorded; probes are task 19 */
        } else if (name == SO_BROADCAST) {
            s->broadcast = v != 0;
        }
        /* The buffers are fixed; a request to size them is accepted,
         * as Linux accepts and clamps one, and getsockopt says what
         * they really are. */
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

int sock_getopt(struct file *f, int level, int name, void *val, u32 *len)
{
    struct socket *s = f->priv;
    int v;

    if (level == IPPROTO_TCP && name == TCP_NODELAY) {
        v = 1;
        goto give_int;
    }
    if (level != SOL_SOCKET) {
        return -ENOPROTOOPT;
    }
    switch (name) {
    case SO_RCVTIMEO:
    case SO_SNDTIMEO: {
        struct timeval tv;
        u32 ms = name == SO_RCVTIMEO ? s->rcvtimeo_ms : s->sndtimeo_ms;

        if (*len < sizeof(tv)) {
            return -EINVAL;
        }
        tv.tv_sec = (s32)(ms / 1000);
        tv.tv_usec = (s32)((ms % 1000) * 1000);
        memcpy(val, &tv, sizeof(tv));
        *len = sizeof(tv);
        return 0;
    }
    case SO_ERROR:
        /*
         * A pending error, cleared by reading it: how a non-blocking
         * connect reports how it ended.
         */
        v = -s->so_error;
        s->so_error = 0;
        if (!v && s->connecting && s->tcp &&
            s->tcp->state != TCP_SYN_SENT) {
            v = -connect_error(s->tcp);
            s->connecting = 0;
        }
        goto give_int;
    case SO_TYPE:
        v = s->type;
        goto give_int;
    case SO_ACCEPTCONN:
        v = s->tcp && s->tcp->state == TCP_LISTEN;
        goto give_int;
    case SO_REUSEADDR:
        v = s->reuseaddr;
        goto give_int;
    case SO_KEEPALIVE:
        v = s->keepalive;
        goto give_int;
    case SO_BROADCAST:
        v = s->broadcast;
        goto give_int;
    case SO_SNDBUF:
        v = s->type == SOCK_STREAM ? TCP_SNDBUF : DGRAM_MAX;
        goto give_int;
    case SO_RCVBUF:
        v = s->type == SOCK_STREAM ? TCP_RCVBUF : DGRAM_SLOTS * DGRAM_MAX;
        goto give_int;
    default:
        return -ENOPROTOOPT;
    }

give_int:
    if (*len < sizeof(int)) {
        return -EINVAL;
    }
    memcpy(val, &v, sizeof(int));
    *len = sizeof(int);
    return 0;
}
