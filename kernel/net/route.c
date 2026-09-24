/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * route.c - the IPv4 routing table.
 *
 * A small array, walked linearly. Sixteen routes is far more than a
 * host with one card needs, and the walk is a handful of instructions
 * per packet -- a hash or a trie would be a data structure earning its
 * keep on a router with a full table, and pretending to on this.
 *
 * The rule is longest-prefix-match: among the routes whose network
 * contains the destination, the one with the most 1-bits in its mask
 * wins, and an equal-length tie goes to the lower metric. That is why a
 * /32 host route overrides the subnet it sits in, and the subnet
 * overrides the 0.0.0.0/0 default -- the default matches everything
 * with a prefix length of zero, so it only wins when nothing else does.
 *
 * Two kinds of route live here together. The INTERFACE routes -- the
 * on-link subnet and the default gateway -- are derived from the
 * address, by route_iface_update, and are torn down and rebuilt when
 * the address changes (a DHCP renewal, say). The STATIC routes are the
 * ones a person added with `route add`; they carry RTF_STATIC and
 * survive a re-address, because throwing away someone's hand-made route
 * because the lease renewed would be a nasty surprise.
 */
#include "route.h"
#include "errno.h"
#include "string.h"

#define ROUTE_MAX 16

struct route {
    ip4_t dest;                 /* network, already masked            */
    ip4_t mask;                 /* 0.0.0.0 is the default route       */
    ip4_t gateway;              /* 0 means on-link (direct)           */
    u32   metric;
    u32   flags;                /* RTF_UP | RTF_GATEWAY | RTF_HOST |
                                 * RTF_STATIC                          */
    u8    lo;                   /* 1: over the loopback interface     */
    u8    used;
};

static struct route table[ROUTE_MAX];

/* Bits set in a mask -- the prefix length, which is what "longest
 * prefix" compares. Contiguous masks are assumed, as everything that
 * writes one here guarantees. */
static int prefix_len(ip4_t mask)
{
    int n = 0;

    while (mask) {
        n += (int)(mask & 1);
        mask >>= 1;
    }
    return n;
}

static struct route *find(ip4_t dest, ip4_t mask)
{
    int i;

    for (i = 0; i < ROUTE_MAX; i++) {
        if (table[i].used && table[i].dest == dest && table[i].mask == mask) {
            return &table[i];
        }
    }
    return 0;
}

static struct route *free_slot(void)
{
    int i;

    for (i = 0; i < ROUTE_MAX; i++) {
        if (!table[i].used) {
            return &table[i];
        }
    }
    return 0;
}

/* The common core of adding a route; callers set RTF_STATIC or not. */
static int add(ip4_t dest, ip4_t mask, ip4_t gw, u32 metric, u32 flags,
               int lo)
{
    struct route *r;

    dest &= mask;                       /* the network, not a host in it */
    if (find(dest, mask)) {
        return -EEXIST;
    }
    r = free_slot();
    if (!r) {
        return -ENOSPC;
    }
    r->dest = dest;
    r->mask = mask;
    r->gateway = gw;
    r->metric = metric;
    r->flags = flags | RTF_UP;
    if (gw) {
        r->flags |= RTF_GATEWAY;
    }
    if (mask == 0xffffffffUL) {
        r->flags |= RTF_HOST;
    }
    r->lo = (u8)lo;
    r->used = 1;
    return 0;
}

void route_init(void)
{
    memset(table, 0, sizeof(table));
    /* Loopback: 127.0.0.0/8 is reachable over lo, on-link, always. */
    add(IP4(127, 0, 0, 0), IP4(255, 0, 0, 0), 0, 0, RTF_UP, 1);
}

int route_lookup(ip4_t dst, ip4_t *nexthop, int *is_lo)
{
    struct route *best = 0;
    int best_len = -1, i;

    for (i = 0; i < ROUTE_MAX; i++) {
        struct route *r = &table[i];
        int len;

        if (!r->used || !(r->flags & RTF_UP)) {
            continue;
        }
        if ((dst & r->mask) != r->dest) {
            continue;
        }
        len = prefix_len(r->mask);
        /* Longer prefix wins; an equal-length tie goes to the lower
         * metric. */
        if (len > best_len ||
            (len == best_len && best && r->metric < best->metric)) {
            best = r;
            best_len = len;
        }
    }
    if (!best) {
        return -ENETUNREACH;
    }
    if (nexthop) {
        *nexthop = best->gateway;
    }
    if (is_lo) {
        *is_lo = best->lo;
    }
    return 0;
}

void route_iface_update(ip4_t ip, ip4_t mask, ip4_t gw)
{
    int i;

    /* Take down the interface's own routes -- but not the loopback
     * route, and not anything a person added by hand. */
    for (i = 0; i < ROUTE_MAX; i++) {
        struct route *r = &table[i];

        if (r->used && !r->lo && !(r->flags & RTF_STATIC)) {
            memset(r, 0, sizeof(*r));
        }
    }
    /* The subnet is reachable on-link; the rest of the world through
     * the gateway, when there is one. */
    if (ip && mask) {
        add(ip & mask, mask, 0, 0, RTF_UP, 0);
    }
    if (gw) {
        add(0, 0, gw, 0, RTF_UP, 0);
    }
}

int route_add(ip4_t dest, ip4_t mask, ip4_t gw, u32 metric, u32 flags)
{
    return add(dest, mask, gw, metric, flags | RTF_STATIC, 0);
}

int route_del(ip4_t dest, ip4_t mask)
{
    struct route *r = find(dest & mask, mask);

    if (!r) {
        return -ESRCH;
    }
    memset(r, 0, sizeof(*r));
    return 0;
}

int route_get(int index, struct routeinfo *out)
{
    int i, seen = 0;

    for (i = 0; i < ROUTE_MAX; i++) {
        if (!table[i].used) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }
        memset(out, 0, sizeof(*out));
        out->dest = table[i].dest;
        out->mask = table[i].mask;
        out->gateway = table[i].gateway;
        out->metric = table[i].metric;
        out->flags = table[i].flags;
        strncpy(out->iface, table[i].lo ? "lo" : "eth0",
                sizeof(out->iface) - 1);
        return 0;
    }
    return -ENOENT;
}
