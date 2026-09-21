/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * net.c - the bottom of the stack: frames in, frames out.
 */
#include "net.h"
#include "dev.h"
#include "tty.h"
#include "timer.h"
#include "console.h"
#include "errno.h"
#include "string.h"

static struct netif iface;

/*
 * The receive ring.
 *
 * Frames are moved off the card into here by net_drain(), from the
 * timer interrupt, and taken out of here by net_poll(), in ordinary
 * kernel context. One producer and one consumer, each writing only its
 * own index, which is what makes it safe without a lock.
 *
 * Sixteen frames is about 24 KB and about a sixth of a second of a busy
 * LAN's broadcast traffic -- enough that the card is always emptied
 * promptly, which is the point, and not so much that a flood can push
 * anything important out of memory.
 */
#define RX_RING 16

struct rxslot {
    u32 len;
    u8  data[NET_MTU];
};

static struct rxslot ring[RX_RING];
static volatile u32 ring_head;          /* written by net_drain only */
static volatile u32 ring_tail;          /* written by net_poll only  */

/*
 * The driver is not reentrant, and net_drain() runs in an interrupt.
 *
 * Receiving and transmitting both bank-switch the chip and both use its
 * pointer register, so an interrupt arriving in the middle of a
 * transmit would leave the chip pointed somewhere else when the
 * transmit resumed. Raising the mask for the length of one send is the
 * cheapest correct answer; the alternative is a flag the interrupt
 * checks, which is the same thing with more ways to be wrong.
 */
static u16 irq_off(void)
{
    u16 sr;

    __asm__ volatile ("move.w %%sr,%0\n\t"
                      "ori.w  #0x0700,%%sr"
                      : "=d"(sr) :: "cc");
    return sr;
}

static void irq_restore(u16 sr)
{
    __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "cc");
}

static void net_poll_idle(void)
{
    net_poll();
}

struct netif *net_if(void)
{
    return &iface;
}

int net_init(void)
{
    struct netdev *d = dev_first_net();
    int err;

    memset(&iface, 0, sizeof(iface));
    if (!d) {
        return -ENODEV;
    }
    iface.dev = d;
    memcpy(iface.mac, d->mac, ETH_ALEN);

    err = d->up(d);
    if (err < 0) {
        return err;
    }
    iface.up = 1;

    arp_init();
    ring_head = ring_tail = 0;

    /*
     * Answer while nobody is asking. Without this the machine responds
     * to a ping only when it happens to be waiting for something of its
     * own, which is not what being on a network means.
     */
    tty_set_idle(net_poll_idle);
    return 0;
}

void net_set_addr(ip4_t ip, ip4_t mask, ip4_t gw)
{
    iface.ip = ip;
    iface.netmask = mask;
    iface.gateway = gw;
}

int net_is_local(ip4_t addr)
{
    if (!iface.netmask) {
        return 0;
    }
    return (addr & iface.netmask) == (iface.ip & iface.netmask);
}

void *net_eth_hdr(void *buf, const u8 *dst, u16 type)
{
    struct ethhdr *e = buf;
    static const u8 bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    memcpy(e->dst, dst ? dst : bcast, ETH_ALEN);
    memcpy(e->src, iface.mac, ETH_ALEN);
    e->type = type;
    return (u8 *)buf + ETH_HDR_LEN;
}

int net_tx(const void *frame, u32 len)
{
    int err;

    if (!iface.up || !iface.dev) {
        return -ENETDOWN;
    }
    {
        u16 sr = irq_off();

        err = iface.dev->send(iface.dev, frame, len);
        irq_restore(sr);
    }
    if (err < 0) {
        iface.tx_errors++;
        return err;
    }
    iface.tx_packets++;
    return 0;
}

/*
 * One frame, dispatched by ethertype.
 *
 * Anything not understood is counted and dropped rather than logged. A
 * machine on a real network sees a great deal it has no interest in --
 * spanning tree, IPv6 router advertisements, other people's broadcasts --
 * and a kernel that printed a line for each would be a kernel nobody
 * could use.
 */
static void net_input(const void *frame, u32 len)
{
    const struct ethhdr *e = frame;

    if (len < ETH_HDR_LEN) {
        iface.rx_dropped++;
        return;
    }
    iface.rx_packets++;

    switch (e->type) {
    case ETH_P_ARP:
        arp_input(frame, len);
        break;

    case ETH_P_IP:
        ip_input(frame, len);
        break;

    default:
        iface.rx_dropped++;
        break;
    }
}

void net_drain(void)
{
    if (!iface.up || !iface.dev) {
        return;
    }

    for (;;) {
        u32 next = (ring_head + 1) % RX_RING;
        s32 n;

        if (next == ring_tail) {
            /*
             * The ring is full, so stop taking frames off the card.
             * They will be dropped by the card instead, which is the
             * right place for it to happen: the card knows how to
             * account for an overrun and this does not.
             */
            return;
        }
        n = iface.dev->recv(iface.dev, ring[ring_head].data, NET_MTU);
        if (n <= 0) {
            return;
        }
        ring[ring_head].len = (u32)n;
        ring_head = next;
    }
}

int net_poll(void)
{
    int handled = 0;

    /*
     * Bounded. An interface being flooded must not be able to hold the
     * kernel here forever -- whatever was waiting on this poll would
     * never get its turn, and on a machine with no scheduler that means
     * the console stops answering.
     */
    while (handled < RX_RING && ring_tail != ring_head) {
        net_input(ring[ring_tail].data, ring[ring_tail].len);
        ring_tail = (ring_tail + 1) % RX_RING;
        handled++;
    }
    return handled;
}

int net_wait(volatile int *flag, u32 ms)
{
    u32 deadline = timer_jiffies() + (ms * HZ + 999) / 1000;

    for (;;) {
        net_poll();
        if (*flag) {
            return 1;
        }
        if ((s32)(timer_jiffies() - deadline) >= 0) {
            return 0;
        }
    }
}
