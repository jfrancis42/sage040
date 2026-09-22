/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tcptest - window scaling, timestamps, SACK, keepalives, TIME_WAIT.
 *
 * All over loopback, both ends this stack, with netctl's knobs making
 * the network misbehave in known ways: a deterministic loss of every
 * Nth data segment, and options switched off to compare against. The
 * comparisons are the point -- "SACK works" is not a thing a test can
 * see, but "with SACK the sender retransmitted less than without it, on
 * the same losses" is.
 */
#include "ulib.h"

#define PORT    6000

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static void info(const char *what, u32 v)
{
    puts("tcptest: ");
    puts(what);
    putch(' ');
    putdec(v);
    putch('\n');
}

static struct sockaddr_in lo(u16 port)
{
    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

#define SA(x)   ((struct sockaddr *)&(x))

/* The connection with these ports, as netstat would show it. */
static int conn(u16 lport, u16 rport, struct conninfo *ci)
{
    u32 i;

    for (i = 0; netctl(NETCTL_CONN, i, ci) == 0; i++) {
        if (ci->local_port == lport && (rport == 0 || ci->remote_port == rport)) {
            return 1;
        }
    }
    return 0;
}

static u16 local_port(int fd)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);

    getsockname(fd, SA(a), &len);
    return ntohs(a.sin_port);
}

static u8 buf[8192];

static u8 pattern(u32 i)
{
    return (u8)(i * 7 + (i >> 11));
}

struct result {
    int  intact;
    u32  got;
    struct conninfo sender;     /* the sending end, after the transfer */
};

/*
 * `bytes` from a child to this process over loopback. The child sends a
 * known pattern, waits until all of it is acknowledged, reports its own
 * connection's counters down a pipe, and closes; this end checks every
 * byte. With `stall_ms`, this end waits that long before reading at all.
 */
static struct result transfer(u32 bytes, u16 port, u32 stall_ms)
{
    struct result r;
    struct sockaddr_in a = lo(port);
    int ls, s, p[2], pid, one = 1, st;
    u32 i;

    memset(&r, 0, sizeof(r));
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, SA(a), sizeof(a));
    listen(ls, 1);
    pipe(p);

    pid = fork();
    if (pid == 0) {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct conninfo ci;
        u32 sent = 0, k;

        close(p[0]);
        if (connect(c, SA(a), sizeof(a)) < 0) {
            exit(1);
        }
        while (sent < bytes) {
            u32 n = bytes - sent > sizeof(buf) ? sizeof(buf) : bytes - sent;
            s32 w;

            for (k = 0; k < n; k++) {
                buf[k] = pattern(sent + k);
            }
            w = write(c, buf, n);
            if (w <= 0) {
                exit(2);
            }
            sent += (u32)w;
            if ((u32)w < n) {
                /* a short write: the rest goes round again */
            }
        }
        /* Everything acknowledged, then the counters, then close. */
        for (k = 0; k < 3000; k++) {
            if (conn(local_port(c), port, &ci) && ci.txq == 0) {
                break;
            }
            msleep(10);
        }
        conn(local_port(c), port, &ci);
        write(p[1], &ci, sizeof(ci));
        close(c);
        exit(0);
    }
    close(p[1]);
    s = accept(ls, 0, 0);
    close(ls);
    if (stall_ms) {
        u32 avail = 0;

        msleep(stall_ms);
        ioctl(s, FIONREAD, (u32)&avail);
        info("unread after the stall:", avail);
        r.got = avail;              /* reported, then read below */
    }
    for (i = 0;;) {
        s32 n = read(s, buf, sizeof(buf));
        s32 k;

        if (n <= 0) {
            break;
        }
        for (k = 0; k < n; k++) {
            if (buf[k] != pattern(i + (u32)k)) {
                i = 0xffffffffu;
                break;
            }
        }
        if (i == 0xffffffffu) {
            break;
        }
        i += (u32)n;
    }
    r.intact = i == bytes;
    if (!stall_ms) {
        r.got = i;
    }
    read(p[0], &r.sender, sizeof(r.sender));
    close(p[0]);
    close(s);
    waitpid(pid, &st, 0);
    return r;
}

static void test_options(void)
{
    struct result r;
    int s = socket(AF_INET, SOCK_STREAM, 0), v;
    socklen_t len = sizeof(v);

    report("SO_RCVBUF is 128 KB",
           getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &len) == 0 && v == 131072);
    close(s);

    r = transfer(256 * 1024, PORT, 0);
    report("256 KB over loopback arrives intact", r.intact);
    report("  with window scaling, timestamps and SACK all agreed",
           (r.sender.flags & (CONN_WS | CONN_TS | CONN_SACK)) ==
           (CONN_WS | CONN_TS | CONN_SACK));
    report("  both shifts 2", r.sender.snd_wscale == 2 &&
                              r.sender.rcv_wscale == 2);
    report("  the peer's window went past what 16 bits can say",
           r.sender.max_snd_wnd > 65535);
    report("  the round trip measured from the timestamps",
           r.sender.rtt_ms > 0);
    report("  and nothing retransmitted, with nothing lost",
           r.sender.rexmit_bytes == 0);
    info("largest window from the peer:", r.sender.max_snd_wnd);

    /* A receiver that does not read: what arrives anyway is the window. */
    r = transfer(200 * 1024, PORT + 1, 1500);
    report("more than 64 KB arrived before the receiver read a byte",
           r.got > 65535);
    report("  and all of it intact", r.intact);
}

static void test_loss(void)
{
    struct result with, without;

    netctl(NETCTL_TCPLOSS, 25, 0);          /* every 25th data segment */
    with = transfer(512 * 1024, PORT + 2, 0);
    netctl(NETCTL_TCPOPTS, TCPOPT_NO_SACK, 0);
    netctl(NETCTL_TCPLOSS, 25, 0);          /* the same losses again */
    without = transfer(512 * 1024, PORT + 3, 0);
    netctl(NETCTL_TCPOPTS, 0, 0);
    netctl(NETCTL_TCPLOSS, 0, 0);

    info("retransmitted with SACK:   ", with.sender.rexmit_bytes);
    info("retransmitted without SACK:", without.sender.rexmit_bytes);
    report("512 KB through one loss in 25 arrives intact, with SACK",
           with.intact && (with.sender.flags & CONN_SACK));
    report("  and without it", without.intact &&
                               !(without.sender.flags & CONN_SACK));
    report("  and both did retransmit, so the losses were real",
           with.sender.rexmit_bytes > 0 && without.sender.rexmit_bytes > 0);
    report("  and SACK resent less than going back N did",
           with.sender.rexmit_bytes < without.sender.rexmit_bytes);
}

static void test_without(void)
{
    struct result r;

    netctl(NETCTL_TCPOPTS, TCPOPT_NO_WS | TCPOPT_NO_TS | TCPOPT_NO_SACK, 0);
    r = transfer(128 * 1024, PORT + 4, 0);
    netctl(NETCTL_TCPOPTS, 0, 0);
    report("with every option off, nothing is agreed",
           (r.sender.flags & (CONN_WS | CONN_TS | CONN_SACK)) == 0);
    report("  the window stays within 16 bits",
           r.sender.max_snd_wnd <= 65535);
    report("  and the transfer is still intact", r.intact);
}

static void test_keepalive(void)
{
    struct sockaddr_in a = lo(PORT + 5);
    struct conninfo ci;
    int ls, c, s, one = 1, v;
    socklen_t len;
    s32 n;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, SA(a), sizeof(a));
    listen(ls, 2);

    c = socket(AF_INET, SOCK_STREAM, 0);
    v = 0;
    report("TCP_KEEPIDLE refuses 0",
           setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &v, sizeof(v)) == -EINVAL);
    setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &one, sizeof(one));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &one, sizeof(one));
    v = 3;
    setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &v, sizeof(v));
    len = sizeof(v);
    report("  and gives back what was set",
           getsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &v, &len) == 0 && v == 3);
    connect(c, SA(a), sizeof(a));
    s = accept(ls, 0, 0);

    /* A live peer: probes go out, are answered, nothing ends. */
    msleep(4500);
    conn(local_port(c), PORT + 5, &ci);
    info("keepalive probes to a live peer:", ci.keep_sent);
    report("an idle connection with keepalive is probed",
           ci.keep_sent >= 2 && (ci.flags & CONN_KEEP));
    report("  and, answered, stays up",
           strcmp(ci.state_name, "ESTABLISHED") == 0 &&
           write(c, "x", 1) == 1 && read(s, buf, 1) == 1);

    /* A dead one: everything vanishes, and after three probes it is
     * given up with ETIMEDOUT. */
    netctl(NETCTL_TCPLOSS, 1, 0);
    msleep(6000);
    netctl(NETCTL_TCPLOSS, 0, 0);
    {
        /* Bounded, so a connection never given up fails here rather
         * than hanging the test. */
        struct timeval tv = { 3, 0 };

        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    n = read(c, buf, 1);
    report("with the peer gone silent, it is given up: ETIMEDOUT",
           n == -ETIMEDOUT);
    close(c);
    close(s);
    close(ls);
}

static void test_timewait(void)
{
    struct sockaddr_in a = lo(PORT + 6), me;
    struct conninfo ci;
    int ls, c, s, one = 1, b;
    u16 port;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, SA(a), sizeof(a));
    listen(ls, 1);
    c = socket(AF_INET, SOCK_STREAM, 0);
    connect(c, SA(a), sizeof(a));
    s = accept(ls, 0, 0);
    port = local_port(c);
    close(c);                   /* the active close: this end waits */
    msleep(300);
    close(s);
    close(ls);

    msleep(15000);              /* past the old 10 s */
    report("TIME_WAIT outlasts fifteen seconds (it was ten)",
           conn(port, PORT + 6, &ci) &&
           strcmp(ci.state_name, "TIME_WAIT") == 0);
    b = socket(AF_INET, SOCK_STREAM, 0);
    me = lo(port);
    report("  and its port cannot be bound without SO_REUSEADDR",
           bind(b, SA(me), sizeof(me)) == -EADDRINUSE);
    close(b);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "timewait") == 0) {
        test_timewait();
    } else {
        test_options();
        test_loss();
        test_without();
        test_keepalive();
        test_timewait();
    }
    puts("tcptest: done\n");
    return 0;
}
