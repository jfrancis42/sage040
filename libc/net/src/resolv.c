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
 * resolv.c - names to addresses, for picolibc programs.
 *
 * In this order, as a Unix resolver does it:
 *
 *   a dotted quad is its own answer;
 *   /etc/hosts, "ADDRESS NAME [ALIAS...]" per line;
 *   "localhost", if /etc/hosts did not say otherwise;
 *   the CACHE -- this process's, of answers DNS gave, each for as long
 *   as its TTL said (and a "no such name" for 30 seconds);
 *   DNS: an A query over UDP to each "nameserver" in /etc/resolv.conf,
 *   or, if it names none, to the server DHCP handed out. Each server is
 *   asked twice, two seconds each, with a fresh random ID, and a reply
 *   is believed only if it comes from where it was sent and carries the
 *   ID and the question asked.
 *
 * "nameserver ADDRESS#PORT" names a port other than 53, as dnsmasq and
 * unbound spell it.
 *
 * The cache is per process. A cache the whole machine shared would need
 * a daemon to hold it; this makes a program that looks the same name up
 * a hundred times ask once, which is most of the benefit.
 */
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

long syscall(long nr, ...);

int h_errno;

#define DNS_PORT      53
#define DNS_TIMEOUT   2         /* seconds, per attempt                */
#define DNS_TRIES     2         /* per server                          */
#define MAX_SERVERS   3
#define CACHE_SIZE    16
#define NEG_TTL       30        /* seconds a "no such name" is kept    */
#define MAX_TTL       86400

#define NR_getrandom  352
#define NR_netctl     1002
#define NETCTL_INFO   0

/* The kernel's struct netinfo (kernel/uapi.h), for the DHCP server. */
struct netinfo {
    char     name[8];
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip, netmask, gateway, up;
    uint32_t rx_packets, tx_packets, rx_dropped, tx_errors;
    uint32_t dns;
};

static int name_eq(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

/* --- the cache ------------------------------------------------------ */

static struct {
    char     name[64];
    uint32_t addr;              /* network order; 0 = "no such name"  */
    time_t   expires;
} cache[CACHE_SIZE];
static unsigned cache_next;
static unsigned long queries_sent;       /* for tests: see __res_queries */

static int cache_find(const char *name, uint32_t *addr)
{
    time_t now = time(0);
    int i;

    for (i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].name[0] && cache[i].expires > now &&
            strlen(name) == strlen(cache[i].name) &&
            name_eq(name, cache[i].name, strlen(name))) {
            *addr = cache[i].addr;
            return 1;
        }
    }
    return 0;
}

static void cache_put(const char *name, uint32_t addr, uint32_t ttl)
{
    unsigned i;

    if (strlen(name) >= sizeof(cache[0].name) || ttl == 0) {
        return;
    }
    if (ttl > MAX_TTL) {
        ttl = MAX_TTL;
    }
    /* Replacing an old answer for the same name first, else the oldest
     * slot, round robin. */
    for (i = 0; i < CACHE_SIZE; i++) {
        if (strlen(name) == strlen(cache[i].name) &&
            name_eq(name, cache[i].name, strlen(name))) {
            break;
        }
    }
    if (i == CACHE_SIZE) {
        i = cache_next++ % CACHE_SIZE;
    }
    strcpy(cache[i].name, name);
    cache[i].addr = addr;
    cache[i].expires = time(0) + (time_t)ttl;
}

/* How many queries this process has put on the wire: what a test of
 * the cache counts. Not in any header -- a program that wants it
 * declares it. */
unsigned long __res_queries(void)
{
    return queries_sent;
}

/* --- small files ---------------------------------------------------- */

static char filebuf[2048];

static int slurp(const char *path)
{
    int fd = open(path, O_RDONLY);
    ssize_t n, total = 0;

    if (fd < 0) {
        return -1;
    }
    while (total < (ssize_t)sizeof(filebuf) - 1 &&
           (n = read(fd, filebuf + total, sizeof(filebuf) - 1 - (size_t)total)) > 0) {
        total += n;
    }
    close(fd);
    filebuf[total] = '\0';
    return (int)total;
}

static char *word(char **pp, size_t *len)
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
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    *len = (size_t)(p - start);
    *pp = p;
    return start;
}

static void next_line(char **pp)
{
    char *p = *pp;

    while (*p && *p != '\n') {
        p++;
    }
    *pp = *p ? p + 1 : p;
}

static int quad(const char *s, size_t len, uint32_t *addr)
{
    char tmp[16];
    struct in_addr a;

    if (len >= sizeof(tmp)) {
        return 0;
    }
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    if (inet_pton(AF_INET, tmp, &a) != 1) {
        return 0;
    }
    *addr = a.s_addr;
    return 1;
}

static int hosts_lookup(const char *name, uint32_t *addr)
{
    char *p = filebuf;
    size_t nlen = strlen(name);

    if (slurp("/etc/hosts") < 0) {
        return 0;
    }
    while (*p) {
        char *w;
        size_t len;
        uint32_t a;

        w = word(&p, &len);
        if (w && quad(w, len, &a)) {
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

struct server {
    uint32_t addr;              /* network order */
    uint16_t port;
};

static int servers(struct server *out)
{
    char *p = filebuf;
    int n = 0;

    if (slurp("/etc/resolv.conf") >= 0) {
        while (*p && n < MAX_SERVERS) {
            char *w;
            size_t len;

            w = word(&p, &len);
            if (w && len == 10 && name_eq(w, "nameserver", 10)) {
                w = word(&p, &len);
                if (w) {
                    size_t i;
                    uint32_t q, port = DNS_PORT;

                    for (i = 0; i < len && w[i] != '#'; i++) {
                    }
                    if (i < len) {
                        size_t k;

                        port = 0;
                        for (k = i + 1; k < len && w[k] >= '0' && w[k] <= '9'; k++) {
                            port = port * 10 + (uint32_t)(w[k] - '0');
                        }
                    }
                    if (quad(w, i, &q) && port > 0 && port < 65536) {
                        out[n].addr = q;
                        out[n].port = (uint16_t)port;
                        n++;
                    }
                }
            }
            next_line(&p);
        }
    }
    if (n == 0) {
        struct netinfo ni;

        memset(&ni, 0, sizeof(ni));
        if (syscall(NR_netctl, NETCTL_INFO, 0, &ni) == 0 && ni.dns) {
            out[0].addr = htonl(ni.dns);
            out[0].port = DNS_PORT;
            n = 1;
        }
    }
    return n;
}

/* --- DNS ------------------------------------------------------------ */

static unsigned char pkt[512];

static int put_name(unsigned char *p, const char *name)
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
        p[n++] = (unsigned char)len;
        memcpy(p + n, name, (size_t)len);
        n += len;
        name = *dot ? dot + 1 : dot;
    }
    p[n++] = 0;
    return n;
}

static int skip_name(const unsigned char *p, int off, int end)
{
    while (off < end) {
        unsigned char len = p[off];

        if (len == 0) {
            return off + 1;
        }
        if ((len & 0xc0) == 0xc0) {
            return off + 2;
        }
        if (len & 0xc0) {
            return -1;
        }
        off += 1 + len;
    }
    return -1;
}

static unsigned get16(const unsigned char *p)
{
    return (unsigned)(p[0] << 8 | p[1]);
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

/* 1 with *addr and *ttl, 0 for "no such name", -1 for no answer. */
static int ask(const struct server *sv, const char *name, uint32_t *addr,
               uint32_t *ttl)
{
    struct sockaddr_in to, from;
    struct timeval tv;
    socklen_t flen;
    uint16_t id;
    int fd, qlen, try;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    tv.tv_sec = DNS_TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(sv->port);
    to.sin_addr.s_addr = sv->addr;

    for (try = 0; try < DNS_TRIES; try++) {
        if (syscall(NR_getrandom, &id, sizeof(id), 0) != sizeof(id)) {
            id = (uint16_t)(time(0) ^ (try << 8));
        }
        memset(pkt, 0, 12);
        pkt[0] = (unsigned char)(id >> 8);
        pkt[1] = (unsigned char)id;
        pkt[2] = 0x01;                      /* recursion desired */
        pkt[5] = 1;                         /* one question      */
        qlen = put_name(pkt + 12, name);
        if (qlen < 0) {
            close(fd);
            return 0;                       /* not a name at all */
        }
        pkt[12 + qlen] = 0;
        pkt[13 + qlen] = 1;                 /* A  */
        pkt[14 + qlen] = 0;
        pkt[15 + qlen] = 1;                 /* IN */
        queries_sent++;
        if (sendto(fd, pkt, (size_t)(16 + qlen), 0, (struct sockaddr *)&to,
                   sizeof(to)) < 0) {
            continue;
        }
        for (;;) {
            unsigned char q[256];
            ssize_t n;
            int off, an, i, rcode;

            flen = sizeof(from);
            n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &flen);
            if (n < 0) {
                break;                      /* timed out: ask again */
            }
            if (from.sin_addr.s_addr != sv->addr || from.sin_port != to.sin_port ||
                n < 12 + qlen + 4 || get16(pkt) != id || !(pkt[2] & 0x80)) {
                continue;                   /* a stray, or a forgery */
            }
            put_name(q, name);
            if (get16(pkt + 4) != 1 ||
                !name_eq((const char *)pkt + 12, (const char *)q, (size_t)qlen)) {
                continue;
            }
            rcode = pkt[3] & 0x0f;
            close(fd);
            if (rcode == 3) {
                return 0;                   /* NXDOMAIN */
            }
            if (rcode != 0) {
                return -1;                  /* SERVFAIL and the like */
            }
            an = (int)get16(pkt + 6);
            off = 12 + qlen + 4;
            for (i = 0; i < an && off < n; i++) {
                unsigned type, cls, rdlen;
                uint32_t t;

                off = skip_name(pkt, off, (int)n);
                if (off < 0 || off + 10 > n) {
                    break;
                }
                type = get16(pkt + off);
                cls = get16(pkt + off + 2);
                t = get32(pkt + off + 4);
                rdlen = get16(pkt + off + 8);
                off += 10;
                if (off + (int)rdlen > n) {
                    break;
                }
                if (type == 1 && cls == 1 && rdlen == 4) {
                    memcpy(addr, pkt + off, 4);
                    *ttl = t;
                    return 1;
                }
                off += (int)rdlen;          /* a CNAME, or anything else */
            }
            return 0;
        }
    }
    close(fd);
    return -1;
}

/*
 * Returns 0 with *addr, or EAI_NONAME, EAI_AGAIN or EAI_FAIL.
 */
static int resolve(const char *name, uint32_t *addr, int numeric_only)
{
    struct server sv[MAX_SERVERS];
    uint32_t a, ttl;
    int n, i, r, last = EAI_AGAIN;
    struct in_addr in;

    if (inet_aton(name, &in)) {
        *addr = in.s_addr;
        return 0;
    }
    if (numeric_only) {
        return EAI_NONAME;
    }
    if (hosts_lookup(name, &a)) {
        *addr = a;
        return 0;
    }
    if (name_eq(name, "localhost", 10)) {
        *addr = htonl(INADDR_LOOPBACK);
        return 0;
    }
    if (cache_find(name, &a)) {
        if (!a) {
            return EAI_NONAME;
        }
        *addr = a;
        return 0;
    }
    n = servers(sv);
    if (n == 0) {
        return EAI_AGAIN;               /* nobody to ask */
    }
    for (i = 0; i < n; i++) {
        r = ask(&sv[i], name, &a, &ttl);
        if (r == 1) {
            cache_put(name, a, ttl);
            *addr = a;
            return 0;
        }
        if (r == 0) {
            cache_put(name, 0, NEG_TTL);
            return EAI_NONAME;
        }
        last = EAI_AGAIN;
    }
    return last;
}

/* --- services ------------------------------------------------------- */

static const struct {
    const char *name;
    int port;
} services[] = {
    { "echo", 7 }, { "ftp", 21 }, { "ssh", 22 }, { "telnet", 23 },
    { "smtp", 25 }, { "domain", 53 }, { "http", 80 }, { "pop3", 110 },
    { "ntp", 123 }, { "imap", 143 }, { "https", 443 },
};
#define NSERVICES (sizeof(services) / sizeof(services[0]))

static int service_port(const char *s, int numeric_only, int *port)
{
    unsigned i;
    long v = 0;
    const char *p = s;

    if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p++ - '0');
            if (v > 65535) {
                return -1;
            }
        }
        if (*p) {
            return -1;
        }
        *port = (int)v;
        return 0;
    }
    if (numeric_only) {
        return -1;
    }
    for (i = 0; i < NSERVICES; i++) {
        if (strcmp(s, services[i].name) == 0) {
            *port = services[i].port;
            return 0;
        }
    }
    return -1;
}

struct servent *getservbyname(const char *name, const char *proto)
{
    static struct servent se;
    static char *none[1];
    int port;

    if (service_port(name, 1, &port) == 0 || service_port(name, 0, &port) < 0) {
        return 0;                       /* a number is not a name */
    }
    se.s_name = (char *)name;
    se.s_aliases = none;
    se.s_port = htons((uint16_t)port);
    se.s_proto = (char *)(proto ? proto : "tcp");
    return &se;
}

struct servent *getservbyport(int port, const char *proto)
{
    static struct servent se;
    static char *none[1];
    unsigned i;

    for (i = 0; i < NSERVICES; i++) {
        if (htons((uint16_t)services[i].port) == port) {
            se.s_name = (char *)services[i].name;
            se.s_aliases = none;
            se.s_port = port;
            se.s_proto = (char *)(proto ? proto : "tcp");
            return &se;
        }
    }
    return 0;
}

/* --- the calls ------------------------------------------------------ */

struct ai_block {
    struct addrinfo    ai;
    struct sockaddr_in sin;
};

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    int port = 0, types[2], ntypes = 0, i, err;
    uint32_t addr;
    struct addrinfo *head = 0, **tail = &head;

    *res = 0;
    if (!node && !service) {
        return EAI_NONAME;
    }
    if (family != AF_UNSPEC && family != AF_INET) {
        return EAI_FAMILY;
    }
    if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) {
        return EAI_SOCKTYPE;
    }
    if (service && service_port(service, flags & AI_NUMERICSERV, &port) < 0) {
        return EAI_SERVICE;
    }
    if (!node) {
        addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    } else {
        err = resolve(node, &addr, flags & AI_NUMERICHOST);
        if (err) {
            return err;
        }
    }

    /* Unspecified: one of each, as glibc gives, stream first. */
    if (socktype == 0 || socktype == SOCK_STREAM) {
        types[ntypes++] = SOCK_STREAM;
    }
    if (socktype == 0 || socktype == SOCK_DGRAM) {
        types[ntypes++] = SOCK_DGRAM;
    }
    for (i = 0; i < ntypes; i++) {
        struct ai_block *b = calloc(1, sizeof(*b));

        if (!b) {
            freeaddrinfo(head);
            return EAI_MEMORY;
        }
        b->sin.sin_family = AF_INET;
        b->sin.sin_port = htons((uint16_t)port);
        b->sin.sin_addr.s_addr = addr;
        b->ai.ai_family = AF_INET;
        b->ai.ai_socktype = types[i];
        b->ai.ai_protocol = protocol ? protocol :
                            types[i] == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
        b->ai.ai_addrlen = sizeof(b->sin);
        b->ai.ai_addr = (struct sockaddr *)&b->sin;
        if (i == 0 && (flags & AI_CANONNAME) && node) {
            b->ai.ai_canonname = strdup(node);
        }
        *tail = &b->ai;
        tail = &b->ai.ai_next;
    }
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;

        free(res->ai_canonname);
        free(res);                      /* the sockaddr is in the block */
        res = next;
    }
}

const char *gai_strerror(int err)
{
    switch (err) {
    case 0:            return "Success";
    case EAI_BADFLAGS: return "Bad value for ai_flags";
    case EAI_NONAME:   return "Name or service not known";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_FAIL:     return "Non-recoverable failure in name resolution";
    case EAI_FAMILY:   return "ai_family not supported";
    case EAI_SOCKTYPE: return "ai_socktype not supported";
    case EAI_SERVICE:  return "Servname not supported for ai_socktype";
    case EAI_MEMORY:   return "Memory allocation failure";
    case EAI_SYSTEM:   return "System error";
    case EAI_OVERFLOW: return "Argument buffer overflow";
    default:           return "Unknown error";
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
                socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    if (!sa || salen < sizeof(*sin) || sin->sin_family != AF_INET) {
        return EAI_FAMILY;
    }
    if (host && hostlen) {
        /* No reverse DNS: the address is its own name, unless a name
         * was required. */
        if (flags & NI_NAMEREQD) {
            return EAI_NONAME;
        }
        if (!inet_ntop(AF_INET, &sin->sin_addr, host, hostlen)) {
            return EAI_OVERFLOW;
        }
    }
    if (serv && servlen) {
        struct servent *se = (flags & NI_NUMERICSERV) ? 0 :
                             getservbyport(sin->sin_port, 0);
        int n;

        if (se) {
            n = snprintf(serv, servlen, "%s", se->s_name);
        } else {
            n = snprintf(serv, servlen, "%u", ntohs(sin->sin_port));
        }
        if (n < 0 || (socklen_t)n >= servlen) {
            return EAI_OVERFLOW;
        }
    }
    return 0;
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static char namebuf[256];
    static uint32_t addr;
    static char *addrs[2], *aliases[1];
    int err;

    err = resolve(name, &addr, 0);
    if (err) {
        h_errno = err == EAI_NONAME ? HOST_NOT_FOUND : TRY_AGAIN;
        return 0;
    }
    strncpy(namebuf, name, sizeof(namebuf) - 1);
    addrs[0] = (char *)&addr;
    addrs[1] = 0;
    aliases[0] = 0;
    he.h_name = namebuf;
    he.h_aliases = aliases;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = addrs;
    return &he;
}

const char *hstrerror(int err)
{
    switch (err) {
    case HOST_NOT_FOUND: return "Unknown host";
    case TRY_AGAIN:      return "Host name lookup failure";
    case NO_RECOVERY:    return "Unknown server error";
    case NO_DATA:        return "No address associated with name";
    default:             return "Unknown resolver error";
    }
}
