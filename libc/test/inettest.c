/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * inettest - picolibc's network layer: sockets, inet_*, getaddrinfo.
 *
 * Run by dnstest.sh, whose DNS server (netservers.py) answers
 * foo.sage.test with 10.1.2.3 and says there is no nosuch.sage.test,
 * and which has written /etc/hosts and /etc/resolv.conf first.
 *
 * Everything here is written the way a program ported from Linux
 * writes it -- <sys/socket.h>, <netdb.h>, getaddrinfo in a loop over
 * the results -- because that is what the layer is for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

unsigned long __res_queries(void);      /* resolv.c: queries sent */

static int failures;

static void report(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static int addr_is(const struct addrinfo *ai, const char *want, int port)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)ai->ai_addr;
    char buf[INET_ADDRSTRLEN];

    return ai->ai_family == AF_INET && ai->ai_addrlen == sizeof(*sin) &&
           inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) &&
           strcmp(buf, want) == 0 && ntohs(sin->sin_port) == port;
}

static void test_text(void)
{
    struct in_addr a;
    char buf[INET_ADDRSTRLEN];
    unsigned char raw[4];

    report("inet_pton reads a dotted quad",
           inet_pton(AF_INET, "192.168.1.20", raw) == 1 &&
           raw[0] == 192 && raw[3] == 20);
    report("  and refuses 256.1.1.1, 1.2.3 and 01.2.3.4",
           inet_pton(AF_INET, "256.1.1.1", raw) == 0 &&
           inet_pton(AF_INET, "1.2.3", raw) == 0 &&
           inet_pton(AF_INET, "01.2.3.4", raw) == 0);
    report("  and says AF_INET6 is not supported",
           inet_pton(AF_INET6, "::1", raw) == -1 && errno == EAFNOSUPPORT);
    report("inet_aton takes the old forms: 127.1 and 0x7f000001",
           inet_aton("127.1", &a) && a.s_addr == htonl(0x7f000001) &&
           inet_aton("0x7f000001", &a) && a.s_addr == htonl(0x7f000001));
    report("inet_addr of garbage is INADDR_NONE",
           inet_addr("not.an.address") == INADDR_NONE);
    a.s_addr = htonl(0x0a000203);
    report("inet_ntop and inet_ntoa write 10.0.2.3",
           strcmp(inet_ntop(AF_INET, &a, buf, sizeof(buf)), "10.0.2.3") == 0 &&
           strcmp(inet_ntoa(a), "10.0.2.3") == 0);
    report("  and inet_ntop refuses a buffer too small",
           inet_ntop(AF_INET, &a, buf, 5) == 0 && errno == ENOSPC);
}

static void test_names(void)
{
    struct addrinfo hints, *res, *ai;
    struct hostent *he;
    struct servent *se;
    unsigned long q0;
    char host[64], serv[16];
    int r, n;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    r = getaddrinfo("10.20.30.40", "8080", &hints, &res);
    report("getaddrinfo of a numeric address and port",
           r == 0 && addr_is(res, "10.20.30.40", 8080) &&
           res->ai_socktype == SOCK_STREAM && res->ai_protocol == IPPROTO_TCP &&
           !res->ai_next);
    if (r == 0) freeaddrinfo(res);

    r = getaddrinfo("localhost", "http", &hints, &res);
    report("localhost, and a service by name", r == 0 && addr_is(res, "127.0.0.1", 80));
    if (r == 0) freeaddrinfo(res);

    r = getaddrinfo("myalias", 0, &hints, &res);
    report("a name from /etc/hosts, by an alias", r == 0 && addr_is(res, "10.4.5.6", 0));
    if (r == 0) freeaddrinfo(res);

    q0 = __res_queries();
    r = getaddrinfo("foo.sage.test", "80", &hints, &res);
    report("a name from DNS", r == 0 && addr_is(res, "10.1.2.3", 80));
    if (r == 0) freeaddrinfo(res);
    report("  which took one query", __res_queries() == q0 + 1);
    r = getaddrinfo("FOO.sage.test", "80", &hints, &res);
    report("  and asked again, in another case, comes from the cache",
           r == 0 && addr_is(res, "10.1.2.3", 80) && __res_queries() == q0 + 1);
    if (r == 0) freeaddrinfo(res);

    /* The server answers spoof.sage.test twice: first a forgery, with
     * the wrong ID and 6.6.6.6, then the truth. */
    r = getaddrinfo("spoof.sage.test", 0, &hints, &res);
    report("a reply with the wrong ID is ignored, not believed",
           r == 0 && addr_is(res, "10.7.7.7", 0));
    if (r == 0) freeaddrinfo(res);

    q0 = __res_queries();
    r = getaddrinfo("nosuch.sage.test", 0, &hints, &res);
    report("a name DNS says does not exist is EAI_NONAME", r == EAI_NONAME);
    r = getaddrinfo("nosuch.sage.test", 0, &hints, &res);
    report("  and so is asking again, without asking the server again",
           r == EAI_NONAME && __res_queries() == q0 + 1);
    report("gai_strerror has words for it",
           strcmp(gai_strerror(EAI_NONAME), "Name or service not known") == 0);

    hints.ai_flags = AI_NUMERICHOST;
    r = getaddrinfo("foo.sage.test", 0, &hints, &res);
    report("AI_NUMERICHOST does not look a name up", r == EAI_NONAME);
    hints.ai_flags = 0;

    memset(&hints, 0, sizeof(hints));
    r = getaddrinfo("127.0.0.1", "53", &hints, &res);
    n = 0;
    for (ai = r == 0 ? res : 0; ai; ai = ai->ai_next) {
        n++;
    }
    report("an unspecified socket type gives stream and datagram",
           r == 0 && n == 2 && res->ai_socktype == SOCK_STREAM &&
           res->ai_next->ai_socktype == SOCK_DGRAM &&
           res->ai_next->ai_protocol == IPPROTO_UDP);
    if (r == 0) freeaddrinfo(res);

    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    r = getaddrinfo(0, "9000", &hints, &res);
    report("AI_PASSIVE with no host is INADDR_ANY", r == 0 && addr_is(res, "0.0.0.0", 9000));
    if (r == 0) freeaddrinfo(res);

    hints.ai_flags = 0;
    hints.ai_family = AF_INET6;
    report("AF_INET6 is EAI_FAMILY", getaddrinfo("localhost", 0, &hints, &res) == EAI_FAMILY);
    hints.ai_family = AF_INET;
    report("an unknown service is EAI_SERVICE",
           getaddrinfo("localhost", "gopher-plus", &hints, &res) == EAI_SERVICE);

    he = gethostbyname("foo.sage.test");
    report("gethostbyname", he && he->h_addrtype == AF_INET && he->h_length == 4 &&
                            ((unsigned char *)he->h_addr)[0] == 10 &&
                            ((unsigned char *)he->h_addr)[3] == 3);
    report("  and a missing name sets h_errno",
           gethostbyname("nosuch.sage.test") == 0 && h_errno == HOST_NOT_FOUND);

    se = getservbyname("https", "tcp");
    report("getservbyname", se && ntohs(se->s_port) == 443);

    {
        struct sockaddr_in sin;

        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(80);
        sin.sin_addr.s_addr = htonl(0x0a010203);
        r = getnameinfo((struct sockaddr *)&sin, sizeof(sin), host, sizeof(host),
                        serv, sizeof(serv), 0);
        report("getnameinfo: the address, and the service by name",
               r == 0 && strcmp(host, "10.1.2.3") == 0 && strcmp(serv, "http") == 0);
        report("  and NI_NAMEREQD, with no reverse DNS, is EAI_NONAME",
               getnameinfo((struct sockaddr *)&sin, sizeof(sin), host, sizeof(host),
                           0, 0, NI_NAMEREQD) == EAI_NONAME);
    }
}

static void test_sockets(void)
{
    struct addrinfo hints, *res;
    struct sockaddr_in me, peer;
    socklen_t len = sizeof(me);
    int ls, c, s, one = 1, r;
    char buf[32];
    struct timeval tv;
    time_t t0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    r = getaddrinfo("127.0.0.1", "0", &hints, &res);

    ls = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    report("TCP: socket, bind, listen through the headers a port uses",
           r == 0 && ls >= 0 && bind(ls, res->ai_addr, res->ai_addrlen) == 0 &&
           listen(ls, 4) == 0 &&
           getsockname(ls, (struct sockaddr *)&me, &len) == 0 &&
           ntohs(me.sin_port) != 0);
    freeaddrinfo(res);

    c = socket(AF_INET, SOCK_STREAM, 0);
    report("  connect", connect(c, (struct sockaddr *)&me, sizeof(me)) == 0);
    len = sizeof(peer);
    s = accept(ls, (struct sockaddr *)&peer, &len);
    report("  accept, with the peer's address",
           s >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    report("  send and recv",
           send(c, "ported", 6, 0) == 6 && recv(s, buf, sizeof(buf), 0) == 6 &&
           memcmp(buf, "ported", 6) == 0);
    report("  TCP_NODELAY can be set", setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                                                  &one, sizeof(one)) == 0);

    /* The timeval conversion: picolibc's tv_sec is 64 bits, the
     * kernel's 32. Unconverted, the kernel reads the high half -- zero
     * -- and the wait does not time out at all, or not when asked. */
    tv.tv_sec = 1;
    tv.tv_usec = 500000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    t0 = time(0);
    r = (int)recv(s, buf, sizeof(buf), 0);
    report("SO_RCVTIMEO of 1.5 s times the wait out: EAGAIN, after 1 or 2 s",
           r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
           time(0) - t0 >= 1 && time(0) - t0 <= 3);
    memset(&tv, 0, sizeof(tv));
    len = sizeof(tv);
    report("  and getsockopt gives it back",
           getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0 &&
           tv.tv_sec == 1 && tv.tv_usec == 500000);

    report("shutdown(SHUT_WR): the other end reads end of file",
           shutdown(c, SHUT_WR) == 0 && recv(s, buf, sizeof(buf), 0) == 0);
    close(c);
    close(s);
    close(ls);

    /* UDP, with sendto and recvfrom. */
    {
        int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a1, from;
        socklen_t fl = sizeof(from);

        memset(&a1, 0, sizeof(a1));
        a1.sin_family = AF_INET;
        a1.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof(a1);
        bind(u1, (struct sockaddr *)&a1, sizeof(a1));
        getsockname(u1, (struct sockaddr *)&a1, &len);
        report("UDP: sendto and recvfrom, with the sender's address",
               sendto(u2, "dgram", 5, 0, (struct sockaddr *)&a1, sizeof(a1)) == 5 &&
               recvfrom(u1, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) == 5 &&
               memcmp(buf, "dgram", 5) == 0 &&
               from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        close(u1);
        close(u2);
    }

    {
        int sv[2];

        report("socketpair",
               socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 &&
               write(sv[0], "pair", 4) == 4 && read(sv[1], buf, 4) == 4 &&
               memcmp(buf, "pair", 4) == 0);
        close(sv[0]);
        close(sv[1]);
    }
}

int main(void)
{
    printf("inettest: sockets and names for picolibc\n");
    test_text();
    test_names();
    test_sockets();
    printf("inettest: %d failed\n", failures);
    return failures != 0;
}
