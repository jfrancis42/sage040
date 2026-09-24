/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * arp.c - turning an IP address into a hardware address.
 *
 * Reference: RFC 826.
 *
 * ARP is the first thing worth implementing on a new network stack and
 * the best thing to test a driver with, because a single exchange
 * exercises everything in both directions: build a frame, hand it to the
 * chip, have the chip put it on the wire, have something out there
 * answer, notice the answer arriving, and read it back. A driver that
 * gets an ARP reply works. One that does not has a fault that no amount
 * of transmit-only testing would find.
 *
 * It is also the first place this machine is visible to anyone else. A
 * host that answers ARP appears in other machines' tables and in the
 * switch's; one that does not is invisible whatever else it implements.
 */
#include "net.h"
#include "route.h"
#include "timer.h"
#include "console.h"
#include "errno.h"
#include "string.h"

#define ARP_HRD_ETHER   1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct arphdr {
    u16 hrd;                    /* hardware type: 1 for ethernet     */
    u16 pro;                    /* protocol type: 0x0800 for IPv4    */
    u8  hln;                    /* hardware address length: 6        */
    u8  pln;                    /* protocol address length: 4        */
    u16 op;
    u8  sha[ETH_ALEN];          /* sender hardware address           */
    u32 spa;                    /* sender protocol address           */
    u8  tha[ETH_ALEN];          /* target hardware address           */
    u32 tpa;                    /* target protocol address           */
} PACKED;

#define ARP_LEN  (ETH_HDR_LEN + (int)sizeof(struct arphdr))

/* --- the cache ------------------------------------------------------ */

/*
 * Small and fixed. A machine on an office LAN talks to a handful of
 * things -- its gateway, a server or two -- and an entry that falls out
 * costs one exchange to get back. Sixteen is more than this has ever
 * needed and fits in 350 bytes.
 */
#define ARP_ENTRIES     16
#define ARP_TTL_MS      120000UL        /* two minutes, as BSD used   */

struct arp_entry {
    ip4_t addr;
    u8    mac[ETH_ALEN];
    u32   stamp;                /* jiffies when it was learned       */
    int   valid;
};

static struct arp_entry cache[ARP_ENTRIES];

/*
 * Set by arp_input when a reply arrives, so that a waiting resolve can
 * stop. volatile because net_wait spins on it and the write happens
 * inside the poll it is driving.
 */
static volatile int got_reply;
static ip4_t waiting_for;

void arp_init(void)
{
    memset(cache, 0, sizeof(cache));
    got_reply = 0;
    waiting_for = 0;
}

static u32 age_ms(const struct arp_entry *e)
{
    return (timer_jiffies() - e->stamp) * (1000 / HZ);
}

static struct arp_entry *lookup(ip4_t addr)
{
    int i;

    for (i = 0; i < ARP_ENTRIES; i++) {
        if (cache[i].valid && cache[i].addr == addr) {
            if (age_ms(&cache[i]) > ARP_TTL_MS) {
                cache[i].valid = 0;
                return 0;
            }
            return &cache[i];
        }
    }
    return 0;
}


/* Remove one entry by address (arp -d). -ENOENT if it is not cached. */
int arp_delete(ip4_t addr)
{
    struct arp_entry *e = lookup(addr);

    if (!e) {
        return -ENOENT;
    }
    e->valid = 0;
    return 0;
}

/* Empty the cache. */
void arp_flush(void)
{
    memset(cache, 0, sizeof(cache));
}

static void learn(ip4_t addr, const u8 *mac)
{
    struct arp_entry *e = lookup(addr);
    int i, oldest = 0;

    if (!e) {
        for (i = 0; i < ARP_ENTRIES; i++) {
            if (!cache[i].valid) {
                e = &cache[i];
                break;
            }
            if (cache[i].stamp < cache[oldest].stamp) {
                oldest = i;
            }
        }
        if (!e) {
            e = &cache[oldest];     /* throw out the least recent */
        }
    }
    e->addr = addr;
    memcpy(e->mac, mac, ETH_ALEN);
    e->stamp = timer_jiffies();
    e->valid = 1;
}

int arp_entry(int index, ip4_t *addr, u8 *mac, u32 *ms)
{
    int i, n = 0;

    for (i = 0; i < ARP_ENTRIES; i++) {
        if (!cache[i].valid) {
            continue;
        }
        if (n++ != index) {
            continue;
        }
        *addr = cache[i].addr;
        memcpy(mac, cache[i].mac, ETH_ALEN);
        *ms = age_ms(&cache[i]);
        return 1;
    }
    return 0;
}

/* --- sending -------------------------------------------------------- */

static int send_arp(u16 op, ip4_t target, const u8 *target_mac,
                    const u8 *eth_dst)
{
    u8 frame[ARP_LEN];
    struct netif *n = net_if();
    struct arphdr *a;
    static const u8 zero[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };

    memset(frame, 0, sizeof(frame));
    a = net_eth_hdr(frame, eth_dst, ETH_P_ARP);

    a->hrd = ARP_HRD_ETHER;
    a->pro = ETH_P_IP;
    a->hln = ETH_ALEN;
    a->pln = 4;
    a->op = op;
    memcpy(a->sha, n->mac, ETH_ALEN);
    a->spa = n->ip;
    memcpy(a->tha, target_mac ? target_mac : zero, ETH_ALEN);
    a->tpa = target;

    return net_tx(frame, sizeof(frame));
}

int arp_request(ip4_t addr)
{
    /* Broadcast, with the target hardware address left zero -- nobody
     * knows it yet, which is the question being asked. */
    return send_arp(ARP_OP_REQUEST, addr, 0, 0);
}

/* --- receiving ------------------------------------------------------- */

void arp_input(const void *frame, u32 len)
{
    const struct arphdr *a = (const struct arphdr *)((const u8 *)frame +
                                                     ETH_HDR_LEN);
    struct netif *n = net_if();

    if (len < (u32)ARP_LEN) {
        return;
    }
    if (a->hrd != ARP_HRD_ETHER || a->pro != ETH_P_IP ||
        a->hln != ETH_ALEN || a->pln != 4) {
        return;
    }

    /*
     * Learn from anything addressed to us, request or reply alike.
     *
     * RFC 826's own optimisation: a host that is asking us for our
     * address is a host we are about to want to answer, so its details
     * are worth keeping whichever direction the packet was going. Note
     * the condition -- only if it is FOR US. Learning from every
     * broadcast on the segment would fill the cache with machines this
     * one has no business talking to.
     */
    if (a->spa && a->tpa == n->ip) {
        learn(a->spa, a->sha);
        if (a->op == ARP_OP_REPLY && a->spa == waiting_for) {
            got_reply = 1;
        }
    }

    /*
     * Answer a request for our own address. This is the half that makes
     * the machine visible: without it nothing on the LAN can send us a
     * packet, however well the rest of the stack works.
     */
    if (a->op == ARP_OP_REQUEST && n->ip && a->tpa == n->ip) {
        send_arp(ARP_OP_REPLY, a->spa, a->sha, a->sha);
    }
}

/* --- resolving ------------------------------------------------------- */

#define ARP_TRIES       3
#define ARP_TIMEOUT_MS  1000

int arp_resolve(ip4_t addr, u8 *mac)
{
    struct arp_entry *e;
    int try;

    if (addr == IP4_BROADCAST || addr == 0) {
        memset(mac, 0xff, ETH_ALEN);
        return 0;
    }

    /*
     * The routing table says which NEIGHBOUR the frame goes to: the
     * destination itself when it is on-link, or a router when it is
     * not. Either way the destination IP is unchanged -- only the
     * ethernet address it is wrapped in. A destination with no route at
     * all is unreachable. This used to be "on my subnet? no: the
     * gateway"; it is a table now so it can be seen and added to (route.c).
     */
    {
        ip4_t nexthop = 0;
        int is_lo = 0, r = route_lookup(addr, &nexthop, &is_lo);

        if (r < 0) {
            return r;           /* -ENETUNREACH */
        }
        if (nexthop) {
            addr = nexthop;     /* through a router */
        }
    }

    e = lookup(addr);
    if (e) {
        memcpy(mac, e->mac, ETH_ALEN);
        return 0;
    }

    for (try = 0; try < ARP_TRIES; try++) {
        got_reply = 0;
        waiting_for = addr;

        if (arp_request(addr) < 0) {
            return -EIO;
        }
        net_wait(&got_reply, ARP_TIMEOUT_MS);
        waiting_for = 0;

        e = lookup(addr);
        if (e) {
            memcpy(mac, e->mac, ETH_ALEN);
            return 0;
        }
    }
    return -EHOSTUNREACH;
}
