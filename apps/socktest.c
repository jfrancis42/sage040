/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * socktest - the socket API, over loopback and socket pairs.
 *
 * Everything here talks to itself -- 127.0.0.1, or the other end of a
 * socketpair -- so it needs no network outside the machine. The host
 * side of TCP is nettest.sh's business.
 */
#include "ulib.h"

#define PORT    5000

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static u32 now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, 0);
    return (u32)tv.tv_sec * 1000 + (u32)tv.tv_usec / 1000;
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

static volatile int got_pipe;

static void on_pipe(int sig)
{
    got_pipe = sig;
}

static u8 big[4096];

static void test_pairs(void)
{
    int sv[2], r, pid, st;
    struct stat stb;
    char b[32];
    struct sockaddr_in any;
    socklen_t len;

    report("socketpair makes two connected ends",
           socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    report("  which fstat calls sockets",
           fstat(sv[0], &stb) == 0 && S_ISSOCK(stb.st_mode));
    report("data goes one way",
           write(sv[0], "ping", 4) == 4 && read(sv[1], b, sizeof(b)) == 4 &&
           memcmp(b, "ping", 4) == 0);
    report("  and the other",
           send(sv[1], "pong", 4, 0) == 4 && recv(sv[0], b, 4, 0) == 4 &&
           memcmp(b, "pong", 4) == 0);

    send(sv[0], "peek", 4, 0);
    report("MSG_PEEK leaves the data there",
           recv(sv[1], b, 4, MSG_PEEK) == 4 && recv(sv[1], b, 4, 0) == 4 &&
           memcmp(b, "peek", 4) == 0);
    report("MSG_DONTWAIT on nothing is EAGAIN",
           recv(sv[1], b, 4, MSG_DONTWAIT) == -EAGAIN);

    len = sizeof(any);
    report("getsockname says AF_UNIX",
           getsockname(sv[0], SA(any), &len) == 0 &&
           any.sin_family == AF_UNIX);

    report("shutdown(SHUT_WR) is end of file for the other end",
           shutdown(sv[0], SHUT_WR) == 0 && read(sv[1], b, 4) == 0);
    report("  while the other direction still works",
           write(sv[1], "back", 4) == 4 && read(sv[0], b, 4) == 4);
    close(sv[0]);
    signal(SIGPIPE, on_pipe);
    got_pipe = 0;
    report("with its peer gone, MSG_NOSIGNAL gives EPIPE and no signal",
           send(sv[1], "x", 1, MSG_NOSIGNAL) == -EPIPE && got_pipe == 0);
    report("  and without it, SIGPIPE as well",
           write(sv[1], "x", 1) == -EPIPE && got_pipe == SIGPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(sv[1]);

    /* A pair across fork: the classic way a parent and child talk. */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid = fork();
    if (pid == 0) {
        s32 n;

        close(sv[0]);
        n = read(sv[1], b, sizeof(b));
        if (n > 0) {
            b[0] = 'E';
            write(sv[1], b, (u32)n);
        }
        exit(0);
    }
    close(sv[1]);
    write(sv[0], "echo", 4);
    r = read(sv[0], b, sizeof(b));
    report("a parent and child talk both ways over a pair",
           r == 4 && memcmp(b, "Echo", 4) == 0);
    waitpid(pid, &st, 0);
    close(sv[0]);

    report("socketpair(AF_INET) is EOPNOTSUPP",
           socketpair(AF_INET, SOCK_STREAM, 0, sv) == -EOPNOTSUPP);
    report("and a datagram pair is EPROTONOSUPPORT",
           socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == -EPROTONOSUPPORT);
}

static void test_tcp(void)
{
    struct sockaddr_in a = lo(PORT), got, peer, mine;
    socklen_t len;
    int ls, c, s, v, pid, st;
    char b[64];
    struct pollfd pf;
    u32 t0, avail;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    report("bind to 127.0.0.1 works", bind(ls, SA(a), sizeof(a)) == 0);
    report("  and listen", listen(ls, 4) == 0);
    len = sizeof(got);
    report("getsockname gives the port back",
           getsockname(ls, SA(got), &len) == 0 && len == sizeof(got) &&
           ntohs(got.sin_port) == PORT);
    v = 0;
    len = sizeof(v);
    report("SO_ACCEPTCONN says it is listening",
           getsockopt(ls, SOL_SOCKET, SO_ACCEPTCONN, &v, &len) == 0 && v == 1);

    fcntl(ls, F_SETFL, O_NONBLOCK);
    report("accept with nothing waiting is EAGAIN, when non-blocking",
           accept(ls, 0, 0) == -EAGAIN);
    fcntl(ls, F_SETFL, 0);

    c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    report("a non-blocking connect is EINPROGRESS",
           connect(c, SA(a), sizeof(a)) == -EINPROGRESS);
    pf.fd = c;
    pf.events = POLLOUT;
    report("  and poll says when it is done",
           poll(&pf, 1, 2000) == 1 && (pf.revents & POLLOUT));
    v = -1;
    len = sizeof(v);
    report("  and SO_ERROR says it worked",
           getsockopt(c, SOL_SOCKET, SO_ERROR, &v, &len) == 0 && v == 0);
    fcntl(c, F_SETFL, 0);

    len = sizeof(peer);
    s = accept(ls, SA(peer), &len);
    report("accept returns the connection", s >= 0);
    len = sizeof(mine);
    getsockname(c, SA(mine), &len);
    report("  and the peer it gives is the client's own address",
           peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
           peer.sin_port == mine.sin_port);
    len = sizeof(got);
    report("getpeername on the client is the server",
           getpeername(c, SA(got), &len) == 0 &&
           ntohs(got.sin_port) == PORT);

    report("data goes across",
           send(c, "hello", 5, 0) == 5 && recv(s, b, sizeof(b), 0) == 5 &&
           memcmp(b, "hello", 5) == 0);
    send(s, "world", 5, 0);
    t0 = now_ms();
    while (ioctl(c, FIONREAD, (u32)&avail) == 0 && avail < 5 &&
           now_ms() - t0 < 1000) {
    }
    report("FIONREAD counts what is waiting", avail == 5);
    report("MSG_PEEK looks without taking",
           recv(c, b, 5, MSG_PEEK) == 5 && recv(c, b, 5, 0) == 5 &&
           memcmp(b, "world", 5) == 0);

    {
        struct timeval tv;

        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        t0 = now_ms();
        report("SO_RCVTIMEO ends a wait with EAGAIN",
               recv(c, b, 5, 0) == -EAGAIN && now_ms() - t0 >= 290);
        tv.tv_usec = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    report("shutdown(SHUT_WR) is end of stream for the peer",
           shutdown(c, SHUT_WR) == 0 && recv(s, b, sizeof(b), 0) == 0);
    report("  and the peer can still send the other way",
           send(s, "still", 5, 0) == 5 && recv(c, b, sizeof(b), 0) == 5);
    close(s);
    close(c);

    /* 100 KB through a child, which counts it. */
    pid = fork();
    if (pid == 0) {
        int k = socket(AF_INET, SOCK_STREAM, 0);
        u32 i;

        connect(k, SA(a), sizeof(a));
        for (i = 0; i < 25; i++) {
            memset(big, 'a' + (int)i, sizeof(big));
            send(k, big, sizeof(big), 0);
        }
        close(k);
        exit(0);
    }
    s = accept(ls, 0, 0);
    {
        u32 total = 0, ok = 1;
        s32 n;

        while ((n = recv(s, big, sizeof(big), 0)) > 0) {
            s32 i;

            for (i = 0; i < n; i++) {
                if (big[i] != 'a' + (int)((total + (u32)i) / 4096)) {
                    ok = 0;
                }
            }
            total += (u32)n;
        }
        report("100 KB from a child arrived whole and in order",
               ok && total == 25 * 4096);
        if (!ok || total != 25 * 4096) {
            puts("    (got ");
            putdec(total);
            puts(ok ? " bytes, in order)\n" : " bytes, OUT OF ORDER)\n");
        }
    }
    close(s);
    waitpid(pid, &st, 0);
    close(ls);

    /* Nobody listening. */
    a = lo(PORT + 1);
    c = socket(AF_INET, SOCK_STREAM, 0);
    report("connect to a port nobody is listening on is ECONNREFUSED",
           connect(c, SA(a), sizeof(a)) == -ECONNREFUSED);
    close(c);
    c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    connect(c, SA(a), sizeof(a));
    pf.fd = c;
    pf.events = POLLOUT;
    poll(&pf, 1, 2000);
    v = 0;
    len = sizeof(v);
    getsockopt(c, SOL_SOCKET, SO_ERROR, &v, &len);
    report("  and non-blocking, SO_ERROR says so", v == ECONNREFUSED);
    close(c);

    /* Port 0, and a port still held. */
    ls = socket(AF_INET, SOCK_STREAM, 0);
    a = lo(0);
    bind(ls, SA(a), sizeof(a));
    len = sizeof(got);
    getsockname(ls, SA(got), &len);
    report("bind to port 0 picks a free one",
           ntohs(got.sin_port) >= 32768);
    close(ls);

    /*
     * A port held by a connection in TIME_WAIT. The SERVER closes first
     * here, so it is the server's end -- on the listening port -- that
     * waits, which is exactly what bites a restarted server.
     */
    a = lo(PORT + 3);
    ls = socket(AF_INET, SOCK_STREAM, 0);
    bind(ls, SA(a), sizeof(a));
    listen(ls, 1);
    c = socket(AF_INET, SOCK_STREAM, 0);
    connect(c, SA(a), sizeof(a));
    s = accept(ls, 0, 0);
    close(s);                           /* the server's FIN first */
    recv(c, b, sizeof(b), 0);           /* the client sees it end */
    close(c);
    close(ls);
    {
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = 200000000;
        nanosleep(&ts, 0);              /* the handshake finishes */
    }
    ls = socket(AF_INET, SOCK_STREAM, 0);
    report("a port held by a connection in TIME_WAIT is EADDRINUSE",
           bind(ls, SA(a), sizeof(a)) == -EADDRINUSE);
    v = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
    report("  and SO_REUSEADDR takes it anyway",
           bind(ls, SA(a), sizeof(a)) == 0 && listen(ls, 1) == 0);
    close(ls);

    a = lo(PORT + 2);
    ls = socket(AF_INET, SOCK_STREAM, 0);
    a.sin_addr.s_addr = htonl(0x0a0b0c0dUL);
    report("binding an address that is not this machine's is EADDRNOTAVAIL",
           bind(ls, SA(a), sizeof(a)) == -EADDRNOTAVAIL);
    close(ls);
}

static void test_udp(void)
{
    struct sockaddr_in a = lo(PORT + 10), from;
    socklen_t len;
    int u, k;
    char b[64];
    struct iovec iov[3];
    struct msghdr m;
    static u8 dg[1473];

    u = socket(AF_INET, SOCK_DGRAM, 0);
    k = socket(AF_INET, SOCK_DGRAM, 0);
    report("a datagram socket binds", bind(u, SA(a), sizeof(a)) == 0);
    sendto(k, "one", 3, 0, SA(a), sizeof(a));
    sendto(k, "two!", 4, 0, SA(a), sizeof(a));
    len = sizeof(from);
    report("datagrams keep their boundaries",
           recvfrom(u, b, sizeof(b), 0, SA(from), &len) == 3 &&
           recvfrom(u, b, sizeof(b), 0, 0, 0) == 4);
    report("  and say who sent them",
           from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && from.sin_port);

    sendto(k, "truncate me", 11, 0, SA(a), sizeof(a));
    report("a short buffer takes the start, and MSG_TRUNC says how long",
           recv(u, b, 4, MSG_TRUNC | MSG_PEEK) == 11 && recv(u, b, 4, 0) == 4 &&
           memcmp(b, "trun", 4) == 0);
    report("  and the rest of that datagram is gone",
           recv(u, b, sizeof(b), MSG_DONTWAIT) == -EAGAIN);

    report("the largest datagram, 1472 bytes, goes",
           sendto(k, dg, 1472, 0, SA(a), sizeof(a)) == 1472 &&
           recv(u, dg, sizeof(dg), 0) == 1472);
    report("  and one more byte is EMSGSIZE",
           sendto(k, dg, 1473, 0, SA(a), sizeof(a)) == -EMSGSIZE);

    report("a connected datagram socket needs no address",
           connect(k, SA(a), sizeof(a)) == 0 && send(k, "conn", 4, 0) == 4 &&
           recv(u, b, sizeof(b), 0) == 4);

    iov[0].iov_base = "sca";
    iov[0].iov_len = 3;
    iov[1].iov_base = "tter";
    iov[1].iov_len = 4;
    memset(&m, 0, sizeof(m));
    m.msg_iov = iov;
    m.msg_iovlen = 2;
    report("sendmsg gathers", sendmsg(k, &m, 0) == 7);
    {
        char p1[2], p2[8];

        iov[0].iov_base = p1;
        iov[0].iov_len = 2;
        iov[1].iov_base = p2;
        iov[1].iov_len = 8;
        memset(&m, 0, sizeof(m));
        m.msg_iov = iov;
        m.msg_iovlen = 2;
        m.msg_name = &from;
        m.msg_namelen = sizeof(from);
        report("recvmsg scatters, and gives the sender",
               recvmsg(u, &m, 0) == 7 && memcmp(p1, "sc", 2) == 0 &&
               memcmp(p2, "atter", 5) == 0 &&
               m.msg_namelen == sizeof(from) && from.sin_port);
    }
    close(u);
    close(k);
}

static void test_options(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), v;
    socklen_t len = sizeof(v);

    report("SO_TYPE says stream",
           getsockopt(s, SOL_SOCKET, SO_TYPE, &v, &len) == 0 &&
           v == SOCK_STREAM);
    len = sizeof(v);
    report("TCP_NODELAY is on: there is no Nagle",
           getsockopt(s, IPPROTO_TCP, TCP_NODELAY, &v, &len) == 0 && v == 1);
    report("an option that does not exist is ENOPROTOOPT",
           setsockopt(s, SOL_SOCKET, 999, &v, sizeof(v)) == -ENOPROTOOPT);
    report("socket(AF_UNIX) is refused: only pairs",
           socket(AF_UNIX, SOCK_STREAM, 0) == -EAFNOSUPPORT);
    report("a socket call on a file is ENOTSOCK",
           listen(0, 1) == -ENOTSOCK);
    close(s);
}

int main(void)
{
    test_pairs();
    test_tcp();
    test_udp();
    test_options();
    puts("socktest: done\n");
    return 0;
}
