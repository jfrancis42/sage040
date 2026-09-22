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
 * its business, and swapping this stack for another means rewriting
 * this file and nothing above it.
 *
 * BLOCKING IS A SPIN, for now. There is no scheduler to sleep against,
 * so a read that has to wait goes round net_wait() -- which drives the
 * protocol while it waits, so the data it is waiting for can actually
 * arrive. That is the same bargain the console has always made. When
 * there are tasks it becomes a sleep on a wait queue and every caller
 * here stays exactly as it is.
 */
#include "net.h"
#include "tcp.h"
#include "vfs.h"
#include "dev.h"
#include "timer.h"
#include "task.h"
#include "signal.h"
#include "errno.h"
#include "string.h"

#define SOCK_MAX        8
#define SOCK_TIMEOUT_MS 30000

struct socket {
    int   used;
    int   type;                 /* SOCK_STREAM or SOCK_DGRAM */
    struct tcpcb *tcp;

    /* Datagram state. A UDP socket has no connection, so what it needs
     * is a bound port and somewhere to put what arrives. */
    u16   port;
    int   bound;
    ip4_t peer_ip;
    u16   peer_port;

    u8    dgram[NET_MTU];
    u32   dgram_len;
    ip4_t dgram_from;
    u16   dgram_port;
    volatile int dgram_ready;
};

static struct socket sockets[SOCK_MAX];

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

/* --- datagrams ------------------------------------------------------ */

static void dgram_arrived(void *arg, ip4_t from, u16 sport,
                          const void *data, u32 len)
{
    struct socket *s = arg;

    if (s->dgram_ready) {
        return;                 /* one at a time; the new one is dropped */
    }
    if (len > sizeof(s->dgram)) {
        len = sizeof(s->dgram);
    }
    memcpy(s->dgram, data, len);
    s->dgram_len = len;
    s->dgram_from = from;
    s->dgram_port = sport;
    s->dgram_ready = 1;
}

/* --- file operations ------------------------------------------------ */

static s32 sock_read(struct file *f, void *buf, u32 len)
{
    struct socket *s = f->priv;
    u32 deadline;

    if (!s || !s->used) {
        return -EBADF;
    }

    if (s->type == SOCK_DGRAM) {
        if (!net_wait(&s->dgram_ready, SOCK_TIMEOUT_MS)) {
            return -ETIMEDOUT;
        }
        if (len > s->dgram_len) {
            len = s->dgram_len;
        }
        memcpy(buf, s->dgram, len);
        s->dgram_ready = 0;
        return (s32)len;
    }

    if (!s->tcp) {
        return -ENOTCONN;
    }

    /*
     * Spin until there is something, the stream ends, or it has been
     * long enough to call it a failure. net_poll() inside the loop is
     * what moves the protocol forward -- without it this would wait for
     * data that nothing was processing.
     */
    deadline = timer_jiffies() + (SOCK_TIMEOUT_MS * HZ) / 1000;
    for (;;) {
        s32 n;

        net_poll();
        tcp_timer();

        n = tcp_recv(s->tcp, buf, len);
        if (n != -EAGAIN) {
            return n;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        if ((s32)(timer_jiffies() - deadline) >= 0) {
            return -ETIMEDOUT;
        }
        net_sleep(20);
    }
}

static s32 sock_write(struct file *f, const void *buf, u32 len)
{
    struct socket *s = f->priv;
    const u8 *p = buf;
    u32 done = 0, deadline;

    if (!s || !s->used) {
        return -EBADF;
    }

    if (s->type == SOCK_DGRAM) {
        if (!s->peer_ip) {
            return -EDESTADDRREQ;
        }
        {
            int err = udp_output(s->peer_ip, s->peer_port, s->port, buf, len);

            return err < 0 ? err : (s32)len;
        }
    }

    if (!s->tcp) {
        return -ENOTCONN;
    }

    /*
     * A short write is legal, but a write() that returns zero having
     * been given data looks to a caller like a closed stream. So this
     * keeps going until at least something has gone, and returns short
     * only after that.
     */
    deadline = timer_jiffies() + (SOCK_TIMEOUT_MS * HZ) / 1000;
    while (done < len) {
        s32 n;

        net_poll();
        tcp_timer();

        n = tcp_send(s->tcp, p + done, len - done);
        if (n < 0) {
            return done > 0 ? (s32)done : n;
        }
        done += (u32)n;
        if (n == 0) {
            if (signal_pending(current)) {
                return done > 0 ? (s32)done : -EINTR;
            }
            if ((s32)(timer_jiffies() - deadline) >= 0) {
                return done > 0 ? (s32)done : -ETIMEDOUT;
            }
            net_sleep(20);
        } else {
            deadline = timer_jiffies() + (SOCK_TIMEOUT_MS * HZ) / 1000;
        }
    }
    return (s32)done;
}

static int sock_ioctl(struct file *f, u32 request, u32 arg)
{
    struct socket *s = f->priv;

    if (!s || !s->used) {
        return -EBADF;
    }
    if (request == FIONREAD) {
        u32 n = 0;

        net_poll();
        if (s->type == SOCK_DGRAM) {
            n = s->dgram_ready ? s->dgram_len : 0;
        } else if (s->tcp) {
            n = tcp_available(s->tcp);
        }
        if (arg) {
            *(u32 *)arg = n;
        }
        return 0;
    }
    return -ENOTTY;
}

static int sock_close(struct file *f)
{
    struct socket *s = f->priv;

    if (!s || !s->used) {
        return -EBADF;
    }
    if (s->type == SOCK_DGRAM) {
        if (s->bound) {
            udp_unbind(s->port);
        }
    } else if (s->tcp) {
        tcp_close(s->tcp);
        /*
         * Give the close a moment to go out and be acknowledged. Not a
         * substitute for a lingering close done properly -- with no
         * scheduler there is nowhere for the connection to live on
         * after the descriptor is gone, so this is the honest amount of
         * effort available.
         */
        {
            u32 deadline = timer_jiffies() + (HZ / 2);

            while ((s32)(timer_jiffies() - deadline) < 0) {
                net_poll();
                tcp_timer();
                net_sleep(20);
            }
        }
        tcp_free(s->tcp);
    }
    s->used = 0;
    return 0;
}


/*
 * A socket is S_IFSOCK. It said S_IFCHR once, back when isatty() was
 * built on S_ISCHR -- which made every socket a terminal.
 */
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

    if (!s || !s->used) {
        return POLLNVAL;
    }
    /* Protocol work happens in ordinary kernel context, whoever asks;
     * a program waiting in poll() is the one asking. */
    net_poll();
    if (s->type == SOCK_DGRAM) {
        return (s->dgram_ready ? POLLIN : 0) | POLLOUT;
    }
    if (!s->tcp) {
        return POLLOUT;         /* unconnected: a write would fail at once */
    }
    return tcp_poll(s->tcp);
}

static const struct file_ops sock_ops = {
    sock_read,
    sock_write,
    0,                          /* a socket is not seekable */
    sock_ioctl,
    sock_close,
    sock_fstat,
    sock_poll,
};

/* --- the system calls ------------------------------------------------ */

int sock_create(int domain, int type, int protocol)
{
    struct socket *s;
    int fd;

    (void)protocol;
    if (domain != AF_INET) {
        return -EAFNOSUPPORT;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
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
    }

    fd = fd_install(&sock_ops, s, O_RDWR);
    if (fd < 0) {
        if (s->tcp) {
            tcp_free(s->tcp);
        }
        s->used = 0;
        return fd;
    }
    return fd;
}

static struct socket *from_fd(int fd)
{
    struct file *f = fd_get(fd);

    if (!f || f->ops != &sock_ops) {
        return 0;
    }
    return f->priv;
}

int sock_bind(int fd, const struct sockaddr_in *addr)
{
    struct socket *s = from_fd(fd);

    if (!s) {
        return -ENOTSOCK;
    }
    if (addr->sin_family != AF_INET) {
        return -EAFNOSUPPORT;
    }

    s->port = addr->sin_port;
    if (s->type == SOCK_DGRAM) {
        int err = udp_bind(s->port, dgram_arrived, s);

        if (err < 0) {
            return err;
        }
        s->bound = 1;
    } else if (s->tcp) {
        s->tcp->local_port = s->port;
    }
    return 0;
}

int sock_connect(int fd, const struct sockaddr_in *addr)
{
    struct socket *s = from_fd(fd);
    u32 deadline;

    if (!s) {
        return -ENOTSOCK;
    }
    if (addr->sin_family != AF_INET) {
        return -EAFNOSUPPORT;
    }

    if (s->type == SOCK_DGRAM) {
        /* No handshake: it only records where write() should send. */
        s->peer_ip = addr->sin_addr;
        s->peer_port = addr->sin_port;
        if (!s->bound) {
            s->port = 32768 + (u16)(timer_jiffies() & 0x7fff);
            if (udp_bind(s->port, dgram_arrived, s) == 0) {
                s->bound = 1;
            }
        }
        return 0;
    }

    {
        int err = tcp_connect(s->tcp, addr->sin_addr, addr->sin_port);

        if (err < 0) {
            return err;
        }
    }

    deadline = timer_jiffies() + (SOCK_TIMEOUT_MS * HZ) / 1000;
    for (;;) {
        net_poll();
        tcp_timer();

        if (s->tcp->state == TCP_ESTABLISHED) {
            return 0;
        }
        if (s->tcp->reset) {
            return -ECONNREFUSED;
        }
        if (s->tcp->state == TCP_CLOSED) {
            return -ETIMEDOUT;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        if ((s32)(timer_jiffies() - deadline) >= 0) {
            return -ETIMEDOUT;
        }
        net_sleep(20);
    }
}

int sock_listen(int fd, int backlog)
{
    struct socket *s = from_fd(fd);

    (void)backlog;
    if (!s) {
        return -ENOTSOCK;
    }
    if (s->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    return tcp_listen(s->tcp, s->port);
}

int sock_accept(int fd, struct sockaddr_in *addr)
{
    struct socket *s = from_fd(fd);
    struct socket *c;
    struct tcpcb *t;
    int newfd;

    if (!s) {
        return -ENOTSOCK;
    }
    if (s->type != SOCK_STREAM || s->tcp->state != TCP_LISTEN) {
        return -EINVAL;
    }

    for (;;) {
        net_poll();
        tcp_timer();

        t = tcp_accept(s->tcp);
        if (t) {
            break;
        }
        /*
         * No timeout. A server waiting for a connection is doing its
         * job, and ctrl-C is what ends it -- which works, because this
         * is inside a system call and the terminal's interrupt is
         * delivered at the boundary.
         */
        if (signal_pending(current)) {
            return -EINTR;
        }
        net_sleep(20);
    }

    c = sock_alloc();
    if (!c) {
        tcp_close(t);
        return -ENFILE;
    }
    c->type = SOCK_STREAM;
    c->tcp = t;

    newfd = fd_install(&sock_ops, c, O_RDWR);
    if (newfd < 0) {
        tcp_close(t);
        c->used = 0;
        return newfd;
    }

    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_addr = t->remote_ip;
        addr->sin_port = t->remote_port;
    }
    return newfd;
}

s32 sock_sendto(int fd, const void *buf, u32 len,
                const struct sockaddr_in *addr)
{
    struct socket *s = from_fd(fd);
    int err;

    if (!s) {
        return -ENOTSOCK;
    }
    if (s->type != SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }
    if (!s->bound) {
        s->port = 32768 + (u16)(timer_jiffies() & 0x7fff);
        if (udp_bind(s->port, dgram_arrived, s) == 0) {
            s->bound = 1;
        }
    }

    err = udp_output(addr->sin_addr, addr->sin_port, s->port, buf, len);
    return err < 0 ? err : (s32)len;
}

s32 sock_recvfrom(int fd, void *buf, u32 len, struct sockaddr_in *addr)
{
    struct socket *s = from_fd(fd);

    if (!s) {
        return -ENOTSOCK;
    }
    if (s->type != SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }
    if (!net_wait(&s->dgram_ready, SOCK_TIMEOUT_MS)) {
        return -ETIMEDOUT;
    }
    if (len > s->dgram_len) {
        len = s->dgram_len;
    }
    memcpy(buf, s->dgram, len);
    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_addr = s->dgram_from;
        addr->sin_port = s->dgram_port;
    }
    s->dgram_ready = 0;
    return (s32)len;
}

int sock_shutdown(int fd, int how)
{
    struct socket *s = from_fd(fd);

    (void)how;
    if (!s) {
        return -ENOTSOCK;
    }
    if (s->type == SOCK_STREAM && s->tcp) {
        tcp_close(s->tcp);
    }
    return 0;
}
