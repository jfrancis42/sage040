/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ip.c - IPv4, in and out.
 *
 * Reference: RFC 791.
 *
 * WHAT THIS DOES NOT DO, said plainly because each one is a decision
 * rather than an omission waiting to be noticed:
 *
 *   No fragmentation, in either direction. An outgoing datagram larger
 *   than the link will fail rather than being split, and an incoming
 *   fragment is dropped rather than held. Reassembly means a buffer
 *   pool, timers and a reassembly queue, all to handle a case that a
 *   1500-byte ethernet almost never produces -- and getting it subtly
 *   wrong is a security bug rather than a performance one. A stack that
 *   refuses is better than a stack that half-reassembles.
 *
 *   No options. The header length is checked and anything longer than
 *   20 bytes is dropped. Options are essentially extinct on the modern
 *   internet and are the source of a long history of parsing bugs.
 *
 *   No routing table. There is one interface, one subnet and one
 *   gateway, so "route" is an if statement. A table would be three
 *   fields and a loop pretending to be a subsystem.
 */
#include "net.h"
#include "timer.h"
#include "errno.h"
#include "string.h"

#define IP_VERSION_4    4
#define IP_MIN_IHL      5       /* 20 bytes, no options */

#define IP_FLAG_MF      0x2000  /* more fragments */
#define IP_FRAG_MASK    0x1fff

struct iphdr {
    u8  vhl;                    /* version in the top nibble, IHL below */
    u8  tos;
    u16 tot_len;
    u16 id;
    u16 frag_off;               /* flags in the top 3 bits              */
    u8  ttl;
    u8  protocol;
    u16 check;
    u32 saddr;
    u32 daddr;
} PACKED;

#define IP_HDR_LEN  ((int)sizeof(struct iphdr))

/*
 * The internet checksum: the one's complement of the one's complement
 * sum of 16-bit words (RFC 1071).
 *
 * The carry has to be folded back in, twice -- the second fold catches
 * the carry the first one can itself produce. Missing that gives a
 * checksum that is right almost always, which is the worst kind of
 * wrong: it passes every casual test and fails on particular data.
 */
u16 net_checksum(const void *data, u32 len, u32 start)
{
    const u8 *p = data;
    u32 sum = start;

    while (len > 1) {
        sum += ((u32)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (u32)p[0] << 8;      /* odd trailing byte, high half */
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (u16)~sum;
}

/* --- out ------------------------------------------------------------ */

static u16 next_id;

int ip_output(ip4_t dst, u8 proto, const void *payload, u32 len)
{
    u8 frame[ETH_HDR_LEN + IP_HDR_LEN + NET_MTU];
    struct netif *n = net_if();
    struct iphdr *ip;
    u8 mac[ETH_ALEN];
    int err;

    /*
     * LOOPBACK. 127/8, and this machine's own address, never go near
     * the wire: the frame is put straight on the loopback queue and
     * net_poll() delivers it as if it had arrived. It works with the
     * interface down or unconfigured, which is exactly when a program
     * talking to itself has to be able to.
     */
    if (IP4_IS_LOOPBACK(dst) || (n->ip && dst == n->ip)) {
        if (len > NET_MTU - ETH_HDR_LEN - (u32)IP_HDR_LEN) {
            return -EMSGSIZE;
        }
        ip = net_eth_hdr(frame, n->mac, ETH_P_IP);
        ip->vhl = (IP_VERSION_4 << 4) | IP_MIN_IHL;
        ip->tos = 0;
        ip->tot_len = (u16)(IP_HDR_LEN + len);
        ip->id = next_id++;
        ip->frag_off = 0;
        ip->ttl = 64;
        ip->protocol = proto;
        ip->check = 0;
        /* To 127.x, from 127.x: the reply comes back the same way. */
        ip->saddr = IP4_IS_LOOPBACK(dst) ? dst : n->ip;
        ip->daddr = dst;
        ip->check = net_checksum(ip, IP_HDR_LEN, 0);
        memcpy((u8 *)ip + IP_HDR_LEN, payload, len);
        return net_loopback(frame, ETH_HDR_LEN + (u32)IP_HDR_LEN + len);
    }

    if (!n->up) {
        return -ENETDOWN;
    }
    /*
     * A datagram may be sent from 0.0.0.0, but only to the broadcast
     * address. That is precisely the DHCP case and nothing else: a host
     * with no address cannot have a conversation with one particular
     * peer, because the peer has nothing to reply to. Refusing it
     * outright is what stopped DHCP from sending a single packet.
     */
    if (!n->ip && dst != IP4_BROADCAST) {
        return -EADDRNOTAVAIL;
    }
    /*
     * Would need fragmenting, which this does not do. NET_MTU is the
     * whole ETHERNET frame, 1514 bytes, so what an IP datagram may carry
     * is that less the Ethernet and IP headers: 1480. This compared
     * against NET_MTU less the IP header alone, which let a datagram
     * build a frame 14 bytes longer than Ethernet allows.
     */
    if (len > NET_MTU - ETH_HDR_LEN - (u32)IP_HDR_LEN) {
        return -EMSGSIZE;
    }

    /*
     * Which hardware address, not which IP. A datagram for somewhere
     * else keeps its destination address and travels in a frame
     * addressed to the gateway -- that split between the two layers is
     * the whole idea of routing, and arp_resolve() is where it happens.
     */
    err = arp_resolve(dst, mac);
    if (err < 0) {
        return err;
    }

    ip = net_eth_hdr(frame, mac, ETH_P_IP);

    ip->vhl = (IP_VERSION_4 << 4) | IP_MIN_IHL;
    ip->tos = 0;
    ip->tot_len = (u16)(IP_HDR_LEN + len);
    ip->id = next_id++;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = proto;
    ip->check = 0;
    ip->saddr = n->ip;
    ip->daddr = dst;
    ip->check = net_checksum(ip, IP_HDR_LEN, 0);

    memcpy((u8 *)ip + IP_HDR_LEN, payload, len);

    return net_tx(frame, ETH_HDR_LEN + (u32)IP_HDR_LEN + len);
}

/* --- in -------------------------------------------------------------- */

void ip_input(const void *frame, u32 len)
{
    const struct iphdr *ip = (const struct iphdr *)((const u8 *)frame +
                                                    ETH_HDR_LEN);
    struct netif *n = net_if();
    u32 avail = len - ETH_HDR_LEN;
    u32 tot, hlen;

    if (avail < (u32)IP_HDR_LEN) {
        return;
    }
    if ((ip->vhl >> 4) != IP_VERSION_4) {
        return;
    }
    hlen = (u32)(ip->vhl & 0x0f) * 4;
    if (hlen != (u32)IP_HDR_LEN) {
        return;                 /* options: not handled, see the top */
    }

    tot = ip->tot_len;
    if (tot < hlen || tot > avail) {
        /*
         * Shorter than its own header, or longer than what arrived.
         * The second is the important one: trusting tot_len over the
         * frame length is how a stack reads past the end of its own
         * buffer.
         */
        return;
    }

    if (net_checksum(ip, hlen, 0) != 0) {
        return;                 /* corrupt, and not ours to complain about */
    }

    if (ip->frag_off & (IP_FLAG_MF | IP_FRAG_MASK)) {
        return;                 /* a fragment; no reassembly */
    }

    /*
     * For us? Accept our own address and broadcasts, and nothing else.
     * A host that answered packets addressed elsewhere would be a
     * router, and an accidental one at that.
     */
    if (ip->daddr != n->ip && !IP4_IS_LOOPBACK(ip->daddr) &&
        ip->daddr != IP4_BROADCAST &&
        !(n->netmask && ip->daddr == (n->ip | ~n->netmask))) {
        return;
    }

    switch (ip->protocol) {
    case IPPROTO_ICMP:
        icmp_input(ip->saddr, (const u8 *)ip + hlen, tot - hlen);
        break;

    case IPPROTO_UDP:
        udp_input(ip->saddr, ip->daddr, (const u8 *)ip + hlen, tot - hlen);
        break;

    case IPPROTO_TCP:
        tcp_input(ip->saddr, ip->daddr, (const u8 *)ip + hlen, tot - hlen);
        break;

    default:
        /* No ICMP protocol-unreachable in reply: answering would tell
         * a scanner exactly which protocols this machine implements. */
        break;
    }
}
