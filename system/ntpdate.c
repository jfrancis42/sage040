/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ntpdate [-q] [-p PORT] SERVER - set the clock from a time server.
 *
 * SNTP, RFC 4330: one request, one reply, and the clock offset worked
 * out from the four timestamps the exchange carries -- when the request
 * left (T1), when the server got it (T2), when the server answered (T3),
 * when the answer arrived (T4):
 *
 *     offset = ((T2 - T1) + (T3 - T4)) / 2
 *
 * which is right to within half the round trip's asymmetry. -q reports
 * the offset without setting anything. The clock is set with
 * settimeofday, which this kernel also writes to the battery-backed
 * clock, so the time survives a reboot.
 */
#include "ulib.h"

#define NTP_PORT     123
#define NTP_UNIX     2208988800UL   /* 1900 to 1970, in seconds */

struct stamp {
    s32 sec;                    /* Unix seconds */
    s32 usec;
};

static u32 get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* An NTP timestamp -- seconds since 1900, and a binary fraction -- as
 * Unix seconds and microseconds. 1e6 / 2^16 is 15625 / 1024, which keeps
 * every step inside 32 bits.
 *
 * NTP's seconds wrap in February 2036. Taking the 1900-to-1970 offset
 * away in unsigned 32-bit arithmetic gets the right Unix time on both
 * sides of the wrap, up to 2106 -- the same unsigned time_t this kernel
 * keeps -- and dnstest.sh checks a server set to 2040. Past 2106 both
 * run out together. */
static struct stamp from_ntp(const u8 *p)
{
    struct stamp s;
    u32 frac = get32(p + 4);

    s.sec = (s32)(get32(p) - NTP_UNIX);
    s.usec = (s32)(((frac >> 16) * 15625UL) >> 10);
    return s;
}

/* And back. 2^32 / 1e6 is 4294.97; 4294 keeps it inside 32 bits and is
 * out by 0.02%, which does not matter: this stamp only has to come
 * back unchanged, as the reply's origin, for the reply to be matched. */
static void to_ntp(struct stamp s, u8 *p)
{
    put32(p, (u32)s.sec + NTP_UNIX);
    put32(p + 4, (u32)s.usec * 4294UL);
}

static struct stamp now(void)
{
    struct timeval tv;
    struct stamp s;

    gettimeofday(&tv, 0);
    s.sec = (s32)tv.tv_sec;
    s.usec = (s32)tv.tv_usec;
    return s;
}

static struct stamp sub(struct stamp a, struct stamp b)
{
    struct stamp d;

    d.sec = a.sec - b.sec;
    d.usec = a.usec - b.usec;
    if (d.usec < 0) {
        d.usec += 1000000;
        d.sec--;
    }
    return d;
}

static struct stamp add(struct stamp a, struct stamp b)
{
    struct stamp d;

    d.sec = a.sec + b.sec;
    d.usec = a.usec + b.usec;
    if (d.usec >= 1000000) {
        d.usec -= 1000000;
        d.sec++;
    }
    return d;
}

/* Halve a (sec, usec) quantity, rounding toward minus infinity. */
static struct stamp half(struct stamp a)
{
    struct stamp d;

    d.sec = a.sec >> 1;
    d.usec = a.usec / 2 + ((a.sec & 1) ? 500000 : 0);
    if (d.usec >= 1000000) {
        d.usec -= 1000000;
        d.sec++;
    }
    return d;
}

static void put_signed(struct stamp d)
{
    /* d is normalised: 0 <= usec < 1e6. Print it as +-S.mmm */
    s32 ms;

    if (d.sec < 0) {
        /* -(sec + usec/1e6) = (-sec - 1) + (1e6 - usec)/1e6 */
        struct stamp m;

        m.sec = -d.sec - (d.usec ? 1 : 0);
        m.usec = d.usec ? 1000000 - d.usec : 0;
        putch('-');
        d = m;
    } else {
        putch('+');
    }
    putdec((u32)d.sec);
    putch('.');
    ms = d.usec / 1000;
    putch((char)('0' + ms / 100));
    putch((char)('0' + ms / 10 % 10));
    putch((char)('0' + ms % 10));
}

int main(int argc, char **argv)
{
    int query = 0, i, fd;
    u32 port = NTP_PORT;
    const char *server = 0;
    struct in_addr addr;
    struct sockaddr_in to, from;
    socklen_t flen;
    struct timeval tv;
    u8 pkt[48];
    struct stamp t1, t2, t3, t4, off, rtt;
    s32 n;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            query = 1;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            const char *p;

            port = 0;
            for (p = argv[++i]; *p >= '0' && *p <= '9'; p++) {
                port = port * 10 + (u32)(*p - '0');
            }
        } else {
            server = argv[i];
        }
    }
    if (!server || port == 0 || port > 65535) {
        eputs("usage: ntpdate [-q] [-p PORT] SERVER\n");
        return 2;
    }
    if (resolve_host(server, &addr) < 0) {
        eputs("ntpdate: cannot resolve ");
        eputs(server);
        eputs("\n");
        return 1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((u16)port);
    to.sin_addr = addr;

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (0 << 6) | (4 << 3) | 3;   /* no leap warning, v4, client */
    t1 = now();
    to_ntp(t1, pkt + 40);               /* transmit: comes back as origin */
    if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&to,
               sizeof(to)) < 0) {
        eputs("ntpdate: cannot send\n");
        return 1;
    }
    for (;;) {
        flen = sizeof(from);
        n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &flen);
        if (n < 0) {
            eputs("ntpdate: no answer from ");
            eputs(server);
            eputs("\n");
            return 1;
        }
        t4 = now();
        /* A server's reply (mode 4), to this request -- its origin
         * timestamp is ours -- with a clock that is synchronised. */
        if (n >= 48 && (pkt[0] & 7) == 4 && pkt[1] != 0 &&
            from.sin_addr.s_addr == addr.s_addr) {
            struct stamp o = from_ntp(pkt + 24);

            if (o.sec == t1.sec) {
                break;
            }
        }
    }
    close(fd);

    t2 = from_ntp(pkt + 32);
    t3 = from_ntp(pkt + 40);
    off = half(add(sub(t2, t1), sub(t3, t4)));
    rtt = sub(sub(t4, t1), sub(t3, t2));

    puts("ntpdate: ");
    puts(server);
    puts(" stratum ");
    putdec(pkt[1]);
    puts(", offset ");
    put_signed(off);
    puts(" s, round trip ");
    put_signed(rtt);
    puts(" s\n");

    if (!query) {
        struct stamp t = add(now(), off);

        tv.tv_sec = (time_t)t.sec;
        tv.tv_usec = (u32)t.usec;
        if (settimeofday(&tv, 0) < 0) {
            eputs("ntpdate: cannot set the clock\n");
            return 1;
        }
        puts("ntpdate: clock set\n");
    }
    return 0;
}
