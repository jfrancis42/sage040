/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * route.h - the IPv4 routing table.
 *
 * Where a datagram goes is a question the network layer answers here,
 * and only here: given a destination, which neighbour does the frame go
 * to -- the destination itself, if it is on a subnet we sit on, or a
 * router that will forward it. That used to be an if statement in
 * arp.c ("on my subnet? no: the gateway"), which is correct for a host
 * with one address and one gateway and nothing else. This makes it a
 * table so that it can be SEEN and CHANGED: a route to another subnet
 * through a second router, a host route, a default that is not DHCP's.
 *
 * Longest prefix wins, ties broken by the lower metric -- the standard
 * rule, so a host route (/32) beats a subnet route beats the default
 * (/0). See route.c.
 */
#ifndef ROUTE_H
#define ROUTE_H

#include "net.h"
#include "uapi.h"       /* struct routeinfo, RTF_* */

/* Clear the table and install the loopback route (127.0.0.0/8 dev lo).
 * Called once, from net_init. */
void route_init(void);

/*
 * The next hop for a destination: 0 in *nexthop means it is on-link
 * (resolve the destination's own hardware address), non-zero means send
 * the frame to that router instead. *is_lo is set for a route over the
 * loopback interface. Returns -ENETUNREACH if nothing matches.
 */
int route_lookup(ip4_t dst, ip4_t *nexthop, int *is_lo);

/*
 * Re-derive the interface's own routes from its address: the on-link
 * route for its subnet and the default route through its gateway. The
 * routes a person added by hand (RTF_STATIC) are left alone, so a DHCP
 * renewal does not wipe them. Called from net_set_addr.
 */
void route_iface_update(ip4_t ip, ip4_t mask, ip4_t gw);

/* Add or remove a route by hand. add: -EEXIST if one for the same
 * dest/mask is there, -ENOSPC if the table is full. del: -ESRCH if
 * there is none. flags carries RTF_STATIC for a route a person added. */
int route_add(ip4_t dest, ip4_t mask, ip4_t gw, u32 metric, u32 flags);
int route_del(ip4_t dest, ip4_t mask);

/* Walk the table for `route` and netstat -r: fills *out, returns 0, or
 * -ENOENT past the last entry. */
int route_get(int index, struct routeinfo *out);

#endif /* ROUTE_H */
