/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * udp.c - datagrams.
 *
 * Reference: RFC 768, which is three pages long and still the whole of
 * it. UDP adds ports and an optional checksum to IP and nothing else,
 * and the shortness of this file is the protocol being honest about
 * that.
 *
 * Delivery is by a small table of bound ports, each with a handler
 * inside the kernel. That is not a sockets layer and is not trying to
 * be: it is what DHCP needs in order to exist, and sockets will be
 * built on top of it once there is something for a program to hold.
 */
#include "net.h"
#include "errno.h"
#include "string.h"

struct udphdr {
    u16 sport;
    u16 dport;
    u16 len;                    /* header and data together */
    u16 check;
} PACKED;

#define UDP_HDR_LEN ((int)sizeof(struct udphdr))

#define UDP_BINDINGS 8

struct binding {
    u16 port;
    udp_handler_t fn;
    void *arg;
};

static struct binding bound[UDP_BINDINGS];

int udp_bind(u16 port, udp_handler_t fn, void *arg)
{
    int i, free_slot = -1;

    for (i = 0; i < UDP_BINDINGS; i++) {
        if (bound[i].fn && bound[i].port == port) {
            return -EADDRINUSE;
        }
        if (!bound[i].fn && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return -ENOSPC;
    }
    bound[free_slot].port = port;
    bound[free_slot].fn = fn;
    bound[free_slot].arg = arg;
    return 0;
}

void udp_unbind(u16 port)
{
    int i;

    for (i = 0; i < UDP_BINDINGS; i++) {
        if (bound[i].fn && bound[i].port == port) {
            bound[i].fn = 0;
        }
    }
}

/*
 * The pseudo-header sum.
 *
 * UDP's checksum covers a header that is not in the packet: the source
 * and destination addresses, the protocol number and the UDP length,
 * all from the IP layer. It exists so that a datagram delivered to the
 * wrong host, or to the right host under the wrong protocol, fails its
 * checksum rather than being accepted -- the transport checking that IP
 * put it where it was meant to go.
 */
static u32 pseudo_sum(ip4_t src, ip4_t dst, u32 udp_len)
{
    return (src >> 16) + (src & 0xffff) +
           (dst >> 16) + (dst & 0xffff) +
           IPPROTO_UDP + udp_len;
}

int udp_output(ip4_t dst, u16 dport, u16 sport, const void *data, u32 len)
{
    u8 buf[UDP_HDR_LEN + 1024];
    struct udphdr *u = (struct udphdr *)buf;
    struct netif *n = net_if();
    u32 total = (u32)UDP_HDR_LEN + len;
    ip4_t src;

    if (len > sizeof(buf) - (u32)UDP_HDR_LEN) {
        return -EMSGSIZE;
    }

    u->sport = sport;
    u->dport = dport;
    u->len = (u16)total;
    u->check = 0;
    memcpy(buf + UDP_HDR_LEN, data, len);

    /*
     * A datagram sent before the interface has an address -- which is
     * exactly what DHCP does -- goes out from 0.0.0.0, and the checksum
     * has to be computed over that same zero.
     */
    src = n->ip;
    u->check = net_checksum(buf, total, pseudo_sum(src, dst, total));
    if (u->check == 0) {
        /*
         * Zero means "no checksum computed", so a real checksum that
         * happens to come out zero is sent as all ones instead. They
         * are the same value in one's complement and only one of them
         * is ambiguous.
         */
        u->check = 0xffff;
    }

    return ip_output(dst, IPPROTO_UDP, buf, total);
}

void udp_input(ip4_t from, const void *data, u32 len)
{
    const struct udphdr *u = data;
    u32 ulen;
    int i;

    if (len < (u32)UDP_HDR_LEN) {
        return;
    }
    ulen = u->len;
    if (ulen < (u32)UDP_HDR_LEN || ulen > len) {
        return;
    }

    /*
     * A zero checksum means the sender did not compute one, which IPv4
     * permits. Anything else has to be right.
     */
    if (u->check != 0) {
        struct netif *n = net_if();

        if (net_checksum(data, ulen,
                         pseudo_sum(from, n->ip, ulen)) != 0) {
            /*
             * Only worth checking against our own address, which a
             * broadcast is not -- so a broadcast whose checksum was
             * computed against the broadcast address will fail here.
             * DHCP sends those, which is why its own datagrams are
             * accepted without this check below.
             */
            if (n->ip != 0) {
                return;
            }
        }
    }

    for (i = 0; i < UDP_BINDINGS; i++) {
        if (bound[i].fn && bound[i].port == u->dport) {
            bound[i].fn(bound[i].arg, from, u->sport,
                        (const u8 *)data + UDP_HDR_LEN,
                        ulen - (u32)UDP_HDR_LEN);
            return;
        }
    }

    /*
     * Nothing bound. No ICMP port-unreachable in reply: a host that
     * answers closed ports tells a scanner precisely which ones are
     * open, and this one has nothing to gain by being helpful.
     */
}
