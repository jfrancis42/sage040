/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * resolv.c - names to addresses.
 *
 * resolve_host("example.org", &addr) the way a Unix resolver does it:
 *
 *   1. a dotted quad is its own answer;
 *   2. /etc/hosts, "ADDRESS NAME [ALIAS...]" per line;
 *   3. "localhost", if /etc/hosts did not say otherwise;
 *   4. DNS: an A query over UDP to each nameserver in /etc/resolv.conf in
 *      turn, or, if that names none, to the one DHCP handed out.
 *
 * The DNS part is RFC 1035's and no more: one question, recursion
 * desired, the first A record in the answer. A CNAME needs no special
 * handling, because a recursive server puts the A records it led to in
 * the same answer. Each server is asked twice, two seconds each, with a
 * fresh random ID -- and an answer is believed only if its ID and its
 * question are the ones asked, which is what keeps a stray or forged
 * datagram from being taken for the reply.
 *
 * "nameserver ADDRESS#PORT" names a port other than 53, as dnsmasq and
 * unbound spell it. The tests use it: nothing unprivileged can listen
 * on 53.
 */
#include "ulib.h"

#define DNS_PORT      53
#define DNS_TIMEOUT   2000      /* ms, per attempt                   */
#define DNS_TRIES     2         /* per server                        */
#define MAX_SERVERS   3

/* --- small helpers -------------------------------------------------- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int name_eq(const char *a, const char *b, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return 0;
        }
    }
    return 1;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Read a whole small file into buf; returns its length or -1. */
static int slurp(const char *path, char *buf, u32 size)
{
    int fd = open(path, O_RDONLY);
    s32 n, total = 0;

    if (fd < 0) {
        return -1;
    }
    while (total < (s32)size - 1 &&
           (n = read(fd, buf + total, size - 1 - (u32)total)) > 0) {
        total += n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

/* The next whitespace-separated word on this line, or 0 at its end. */
static char *word(char **pp, u32 *len)
{
    char *p = *pp, *start;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '#') {
        *pp = p;
        return 0;
    }
    start = p;
    while (*p && !is_space(*p)) {
        p++;
    }
    *len = (u32)(p - start);
    *pp = p;
    return start;
}

static void next_line(char **pp)
{
    char *p = *pp;

    while (*p && *p != '\n') {
        p++;
    }
    if (*p == '\n') {
        p++;
    }
    *pp = p;
}

static int parse_quad(const char *s, u32 len, u32 *addr)
{
    char tmp[16];
    struct in_addr in;

    if (len >= sizeof(tmp)) {
        return 0;
    }
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    if (!inet_aton(tmp, &in)) {
        return 0;
    }
    *addr = in.s_addr;
    return 1;
}

/* --- /etc/hosts ------------------------------------------------------ */

static char filebuf[2048];

static int hosts_lookup(const char *name, u32 *addr)
{
    char *p = filebuf;
    u32 nlen = (u32)strlen(name);

    if (slurp("/etc/hosts", filebuf, sizeof(filebuf)) < 0) {
        return 0;
    }
    while (*p) {
        char *w;
        u32 len, a;

        w = word(&p, &len);
        if (w && parse_quad(w, len, &a)) {
            while ((w = word(&p, &len)) != 0) {
                if (len == nlen && name_eq(w, name, len)) {
                    *addr = a;
                    return 1;
                }
            }
        }
        next_line(&p);
    }
    return 0;
}

/* --- where to ask ---------------------------------------------------- */

struct server {
    u32 addr;                   /* network order */
    u16 port;
};

static int servers(struct server *out)
{
    char *p = filebuf;
    int n = 0;

    if (slurp("/etc/resolv.conf", filebuf, sizeof(filebuf)) >= 0) {
        while (*p && n < MAX_SERVERS) {
            char *w;
            u32 len;

            w = word(&p, &len);
            if (w && len == 10 && name_eq(w, "nameserver", 10)) {
                w = word(&p, &len);
                if (w) {
                    u32 q = 0, port = DNS_PORT, i;

                    for (i = 0; i < len && w[i] != '#'; i++) {
                    }
                    if (i < len) {
                        u32 k;

                        port = 0;
                        for (k = i + 1; k < len && w[k] >= '0' && w[k] <= '9'; k++) {
                            port = port * 10 + (u32)(w[k] - '0');
                        }
                    }
                    if (parse_quad(w, i, &q) && port > 0 && port < 65536) {
                        out[n].addr = q;
                        out[n].port = (u16)port;
                        n++;
                    }
                }
            }
            next_line(&p);
        }
    }
    if (n == 0) {
        /* Nothing configured: DHCP's, if it handed one out. */
        struct netinfo ni;

        if (netctl(NETCTL_INFO, 0, &ni) == 0 && ni.dns) {
            out[0].addr = htonl(ni.dns);
            out[0].port = DNS_PORT;
            n = 1;
        }
    }
    return n;
}

/* --- the protocol ---------------------------------------------------- */

static u8 pkt[512];

/* "www.example.org" as DNS labels. Returns the length, or -1. */
static int put_name(u8 *p, const char *name)
{
    int n = 0;

    while (*name) {
        const char *dot = name;
        int len;

        while (*dot && *dot != '.') {
            dot++;
        }
        len = (int)(dot - name);
        if (len == 0 || len > 63 || n + len + 2 > 255) {
            return -1;
        }
        p[n++] = (u8)len;
        memcpy(p + n, name, (u32)len);
        n += len;
        name = *dot ? dot + 1 : dot;
    }
    p[n++] = 0;
    return n;
}

/* Step over a name in a reply, following no pointers -- only its length
 * in this place matters. Returns the offset after it, or -1. */
static int skip_name(const u8 *p, int off, int end)
{
    while (off < end) {
        u8 len = p[off];

        if (len == 0) {
            return off + 1;
        }
        if ((len & 0xc0) == 0xc0) {
            return off + 2;     /* a pointer ends the name here */
        }
        if (len & 0xc0) {
            return -1;
        }
        off += 1 + len;
    }
    return -1;
}

static u16 get16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}

/*
 * Ask one server. Returns 1 with *addr set, 0 for "no such name", or a
 * negative errno for no answer.
 */
static int ask(const struct server *sv, const char *name, u32 *addr)
{
    struct sockaddr_in to, from;
    struct timeval tv;
    socklen_t flen;
    u16 id;
    int fd, qlen, try, err = -ETIMEDOUT;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return fd;
    }
    tv.tv_sec = DNS_TIMEOUT / 1000;
    tv.tv_usec = (DNS_TIMEOUT % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(sv->port);
    to.sin_addr.s_addr = sv->addr;

    for (try = 0; try < DNS_TRIES; try++) {
        s32 n;

        syscall(__NR_getrandom, (u32)&id, sizeof(id), 0);
        memset(pkt, 0, 12);
        pkt[0] = (u8)(id >> 8);
        pkt[1] = (u8)id;
        pkt[2] = 0x01;              /* RD: recursion desired */
        pkt[5] = 1;                 /* one question          */
        qlen = put_name(pkt + 12, name);
        if (qlen < 0) {
            close(fd);
            return -EINVAL;
        }
        pkt[12 + qlen] = 0;
        pkt[13 + qlen] = 1;         /* QTYPE A  */
        pkt[14 + qlen] = 0;
        pkt[15 + qlen] = 1;         /* QCLASS IN */
        if (sendto(fd, pkt, (u32)(16 + qlen), 0, (struct sockaddr *)&to,
                   sizeof(to)) < 0) {
            err = -EHOSTUNREACH;
            continue;
        }

        for (;;) {
            u8 q[256];
            int off, an, i, rcode;

            flen = sizeof(from);
            n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from,
                         &flen);
            if (n < 0) {
                break;              /* timed out: ask again */
            }
            /* From the server asked, answering this question -- else a
             * stray, and waiting goes on. */
            if (from.sin_addr.s_addr != sv->addr ||
                from.sin_port != to.sin_port || n < 12 + qlen + 4 ||
                get16(pkt) != id || !(pkt[2] & 0x80)) {
                continue;
            }
            put_name(q, name);
            if (get16(pkt + 4) != 1 ||
                !name_eq((const char *)pkt + 12, (const char *)q, (u32)qlen)) {
                continue;
            }
            rcode = pkt[3] & 0x0f;
            if (rcode == 3) {
                close(fd);
                return 0;           /* NXDOMAIN: there is no such name */
            }
            if (rcode != 0) {
                err = -EIO;         /* SERVFAIL and the like: next server */
                close(fd);
                return err;
            }
            an = get16(pkt + 6);
            off = 12 + qlen + 4;
            for (i = 0; i < an && off < n; i++) {
                int type, cls, rdlen;

                off = skip_name(pkt, off, n);
                if (off < 0 || off + 10 > n) {
                    break;
                }
                type = get16(pkt + off);
                cls = get16(pkt + off + 2);
                rdlen = get16(pkt + off + 8);
                off += 10;
                if (off + rdlen > n) {
                    break;
                }
                if (type == 1 && cls == 1 && rdlen == 4) {
                    memcpy(addr, pkt + off, 4);
                    close(fd);
                    return 1;
                }
                off += rdlen;       /* a CNAME, or anything else */
            }
            close(fd);
            return 0;               /* an answer with no address in it */
        }
    }
    close(fd);
    return err;
}

int resolve_host(const char *name, struct in_addr *out)
{
    struct server sv[MAX_SERVERS];
    int n, i, r, last = -ENOENT;
    u32 a;

    if (inet_aton(name, out)) {
        return 0;
    }
    if (hosts_lookup(name, &a)) {
        out->s_addr = a;
        return 0;
    }
    if (strcmp(name, "localhost") == 0) {
        out->s_addr = htonl(INADDR_LOOPBACK);
        return 0;
    }
    n = servers(sv);
    if (n == 0) {
        return -ENETUNREACH;        /* nobody to ask */
    }
    for (i = 0; i < n; i++) {
        r = ask(&sv[i], name, &a);
        if (r == 1) {
            out->s_addr = a;
            return 0;
        }
        if (r == 0) {
            return -ENOENT;         /* an authoritative "no" */
        }
        last = r;
    }
    return last;
}
