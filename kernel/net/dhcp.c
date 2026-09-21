/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * dhcp.c - asking the network what this machine's address is.
 *
 * Reference: RFC 2131, and RFC 2132 for the options.
 *
 * DISCOVER, OFFER, REQUEST, ACK. The four-way exchange exists because
 * more than one server may answer: the client broadcasts what it wants,
 * every server offers, and the client broadcasts which offer it took so
 * that the servers who lost can put their addresses back. Doing only
 * the first half -- taking the first OFFER and using it -- works on a
 * network with one server and quietly leaks addresses on a network with
 * two.
 *
 * WHY BROADCAST THROUGHOUT: the client has no address yet, so it cannot
 * receive a unicast reply -- there is nothing for the server to ARP for.
 * Everything goes to 255.255.255.255 from 0.0.0.0 until the lease is
 * accepted. That is also why udp.c has to be willing to send from a
 * zero source address.
 *
 * WHAT IS NOT DONE: the lease is taken and not renewed. A renewal is a
 * unicast REQUEST at half the lease time, which needs a timer running
 * against something that is not there yet; until then a long-running
 * machine will keep using an address after its lease expires. On a
 * network that hands out week-long leases nobody notices, and saying so
 * is better than a renewal path that has never once run.
 */
#include "net.h"
#include "timer.h"
#include "console.h"
#include "errno.h"
#include "string.h"

#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

#define BOOTREQUEST         1
#define BOOTREPLY           2

#define HTYPE_ETHER         1

#define DHCP_MAGIC          0x63825363UL

/* Message types, option 53. */
#define DHCPDISCOVER        1
#define DHCPOFFER           2
#define DHCPREQUEST         3
#define DHCPACK             5
#define DHCPNAK             6

/* Options used here. */
#define OPT_PAD             0
#define OPT_NETMASK         1
#define OPT_ROUTER          3
#define OPT_DNS             6
#define OPT_HOSTNAME        12
#define OPT_REQUESTED_IP    50
#define OPT_LEASE_TIME      51
#define OPT_MSG_TYPE        53
#define OPT_SERVER_ID       54
#define OPT_PARAM_LIST      55
#define OPT_END             255

struct dhcp_msg {
    u8  op;
    u8  htype;
    u8  hlen;
    u8  hops;
    u32 xid;
    u16 secs;
    u16 flags;
    u32 ciaddr;                 /* client's own, if it has one        */
    u32 yiaddr;                 /* "your" address -- what is offered  */
    u32 siaddr;
    u32 giaddr;
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u32 magic;
    u8  options[312];
} PACKED;

/* What the exchange has learned so far. */
static struct {
    volatile int got;           /* a message we were waiting for      */
    int   type;                 /* which one                          */
    u32   xid;
    ip4_t offered;
    ip4_t server;
    ip4_t netmask;
    ip4_t router;
    ip4_t dns;
    u32   lease;
} state;

/* --- options -------------------------------------------------------- */

static u8 *opt_put(u8 *p, u8 code, u8 len, const void *val)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

static u8 *opt_put_u8(u8 *p, u8 code, u8 v)
{
    *p++ = code;
    *p++ = 1;
    *p++ = v;
    return p;
}

static u8 *opt_put_ip(u8 *p, u8 code, ip4_t v)
{
    u8 b[4];

    b[0] = (u8)(v >> 24);
    b[1] = (u8)(v >> 16);
    b[2] = (u8)(v >> 8);
    b[3] = (u8)v;
    return opt_put(p, code, 4, b);
}

static ip4_t opt_ip(const u8 *p)
{
    return IP4(p[0], p[1], p[2], p[3]);
}

/*
 * Walk the option block.
 *
 * Bounded by the buffer as well as by OPT_END, because a truncated or
 * hostile packet can simply not contain the terminator -- and a parser
 * that trusts it to walks off the end of the datagram.
 */
static void parse_options(const u8 *p, u32 len)
{
    u32 i = 0;

    state.type = 0;
    state.netmask = 0;
    state.router = 0;
    state.dns = 0;
    state.server = 0;
    state.lease = 0;

    while (i < len) {
        u8 code = p[i];
        u8 olen;

        if (code == OPT_END) {
            return;
        }
        if (code == OPT_PAD) {
            i++;
            continue;
        }
        if (i + 2 > len) {
            return;
        }
        olen = p[i + 1];
        if (i + 2 + olen > len) {
            return;
        }

        switch (code) {
        case OPT_MSG_TYPE:
            if (olen >= 1) {
                state.type = p[i + 2];
            }
            break;
        case OPT_NETMASK:
            if (olen >= 4) {
                state.netmask = opt_ip(&p[i + 2]);
            }
            break;
        case OPT_ROUTER:
            if (olen >= 4) {
                state.router = opt_ip(&p[i + 2]);
            }
            break;
        case OPT_DNS:
            if (olen >= 4) {
                state.dns = opt_ip(&p[i + 2]);
            }
            break;
        case OPT_SERVER_ID:
            if (olen >= 4) {
                state.server = opt_ip(&p[i + 2]);
            }
            break;
        case OPT_LEASE_TIME:
            if (olen >= 4) {
                state.lease = ((u32)p[i + 2] << 24) | ((u32)p[i + 3] << 16) |
                              ((u32)p[i + 4] << 8) | (u32)p[i + 5];
            }
            break;
        default:
            break;
        }
        i += 2 + olen;
    }
}

/* --- sending -------------------------------------------------------- */

static int send_msg(u8 type, ip4_t requested, ip4_t server)
{
    static struct dhcp_msg m;   /* 576 bytes: too big for the stack */
    struct netif *n = net_if();
    u8 *p;
    static const u8 wanted[] = { OPT_NETMASK, OPT_ROUTER, OPT_DNS };
    static const char host[] = "sage040";

    memset(&m, 0, sizeof(m));
    m.op = BOOTREQUEST;
    m.htype = HTYPE_ETHER;
    m.hlen = ETH_ALEN;
    m.xid = state.xid;
    /*
     * The broadcast flag, asking the server to reply to the broadcast
     * address rather than unicast to an address we do not have yet.
     * A server that ignores it would ARP for an address nobody answers
     * to, and the reply would never arrive.
     */
    m.flags = 0x8000;
    memcpy(m.chaddr, n->mac, ETH_ALEN);
    m.magic = DHCP_MAGIC;

    p = m.options;
    p = opt_put_u8(p, OPT_MSG_TYPE, type);
    if (requested) {
        p = opt_put_ip(p, OPT_REQUESTED_IP, requested);
    }
    if (server) {
        p = opt_put_ip(p, OPT_SERVER_ID, server);
    }
    p = opt_put(p, OPT_PARAM_LIST, sizeof(wanted), wanted);
    p = opt_put(p, OPT_HOSTNAME, sizeof(host) - 1, host);
    *p++ = OPT_END;

    return udp_output(IP4_BROADCAST, DHCP_SERVER_PORT, DHCP_CLIENT_PORT,
                      &m, (u32)(p - (u8 *)&m));
}

/* --- receiving ------------------------------------------------------ */

static void dhcp_recv(void *arg, ip4_t from, u16 sport,
                      const void *data, u32 len)
{
    const struct dhcp_msg *m = data;
    struct netif *n = net_if();

    (void)arg;
    (void)from;
    (void)sport;

    if (len < 240) {            /* everything up to and including magic */
        return;
    }
    if (m->op != BOOTREPLY || m->magic != DHCP_MAGIC) {
        return;
    }
    if (m->xid != state.xid) {
        return;                 /* somebody else's conversation */
    }
    if (memcmp(m->chaddr, n->mac, ETH_ALEN) != 0) {
        return;
    }

    parse_options(m->options, len - 240);
    if (state.type == 0) {
        return;
    }
    state.offered = m->yiaddr;
    state.got = 1;
}

/* --- the exchange --------------------------------------------------- */

#define DHCP_TIMEOUT_MS 4000
#define DHCP_TRIES      3

int dhcp_configure(void)
{
    struct netif *n = net_if();
    int try, err;

    if (!n->up) {
        return -ENETDOWN;
    }

    err = udp_bind(DHCP_CLIENT_PORT, dhcp_recv, 0);
    if (err < 0 && err != -EADDRINUSE) {
        return err;
    }

    /*
     * The address is cleared first. A machine that already had one
     * would otherwise send its DISCOVER from that address, and receive
     * the reply only if the server happened to unicast it back --
     * which is exactly the case the broadcast flag exists to avoid.
     */
    net_set_addr(0, 0, 0);

    for (try = 0; try < DHCP_TRIES; try++) {
        /*
         * A fresh transaction id each attempt, seeded from the clock
         * and the MAC so that two machines starting together do not
         * collide. There is no random number generator here and this
         * does not need one -- it needs to be different, not
         * unguessable.
         */
        state.xid = (timer_jiffies() << 8) ^
                    ((u32)n->mac[3] << 16) ^ ((u32)n->mac[4] << 8) ^
                    n->mac[5] ^ ((u32)try << 24);

        state.got = 0;
        if (send_msg(DHCPDISCOVER, 0, 0) < 0) {
            continue;
        }
        if (!net_wait(&state.got, DHCP_TIMEOUT_MS)) {
            continue;           /* nobody answered; try again */
        }
        if (state.type != DHCPOFFER || !state.offered) {
            continue;
        }

        {
            ip4_t offered = state.offered;
            ip4_t server = state.server;
            ip4_t mask = state.netmask;
            ip4_t router = state.router;

            state.got = 0;
            if (send_msg(DHCPREQUEST, offered, server) < 0) {
                continue;
            }
            if (!net_wait(&state.got, DHCP_TIMEOUT_MS)) {
                continue;
            }
            if (state.type == DHCPNAK) {
                continue;       /* somebody else got it; start over */
            }
            if (state.type != DHCPACK) {
                continue;
            }

            /*
             * The ACK's own options win where it has them -- it is the
             * authoritative answer and the OFFER was a proposal -- but
             * a server that repeats only some of them is common enough
             * that the offer's values are kept as a fallback.
             */
            if (state.netmask) {
                mask = state.netmask;
            }
            if (state.router) {
                router = state.router;
            }
            if (!mask) {
                /* No netmask at all: assume the class the address
                 * falls in, which is what BOOTP clients did. */
                u8 top = (u8)(state.offered >> 24);

                mask = top < 128 ? 0xff000000UL :
                       top < 192 ? 0xffff0000UL : 0xffffff00UL;
            }

            net_set_addr(state.offered, mask, router);
            udp_unbind(DHCP_CLIENT_PORT);
            return 0;
        }
    }

    udp_unbind(DHCP_CLIENT_PORT);
    return -ETIMEDOUT;
}

u32 dhcp_lease_seconds(void)
{
    return state.lease;
}

ip4_t dhcp_server(void)
{
    return state.server;
}

ip4_t dhcp_dns(void)
{
    return state.dns;
}
