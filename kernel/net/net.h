/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * net.h - the network stack, such as it is.
 *
 * Sits on `struct netdev` and knows nothing about the LAN91C111 -- the
 * driver hands it whole ethernet frames and takes whole ethernet frames
 * back, and swapping the chip changes nothing above this line.
 *
 * BYTE ORDER IS THE EASY CASE HERE, and it is worth saying why so that
 * nobody adds a swap out of habit. Network byte order IS big-endian, and
 * so is this machine, so htons and ntohl are the identity function and a
 * whole category of porting bug does not arise. The contrast is with
 * this board's own devices: the ATA data register and every SM501
 * register are little-endian. Those are device quirks and they stop at
 * their drivers.
 *
 * UNALIGNED ACCESS IS ALSO THE EASY CASE. Protocol headers are full of
 * 32-bit fields at odd offsets -- an IP header begins 14 bytes into a
 * frame, so its addresses land 2-aligned and never 4-aligned. On most
 * architectures that means packed-struct gymnastics or byte-at-a-time
 * accessors. The 68040 does it in hardware for a cycle penalty. The
 * structures below are still marked packed, because that is about what
 * the COMPILER would otherwise insert, not about what the CPU can do:
 * without it GCC pads `struct iphdr` to align its addresses and the
 * structure stops describing the wire.
 */
#ifndef NET_H
#define NET_H

#include "kernel.h"
#include "dev.h"

#define ETH_ALEN        6
#define ETH_HDR_LEN     14
#define ETH_MIN         60      /* without the FCS; the chip pads */

#define ETH_P_IP        0x0800
#define ETH_P_ARP       0x0806

#define PACKED __attribute__((packed))

struct ethhdr {
    u8  dst[ETH_ALEN];
    u8  src[ETH_ALEN];
    u16 type;
} PACKED;

/* --- addresses ------------------------------------------------------ */

/*
 * An IPv4 address is kept as a u32 in host order, which on this machine
 * is also wire order. Converting at the edges anyway would be two
 * no-ops and one more thing to get backwards.
 */
typedef u32 ip4_t;

#define IP4(a, b, c, d) \
    (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

#define IP4_ANY         0x00000000UL
#define IP4_BROADCAST   0xffffffffUL
#define IP4_IS_LOOPBACK(a)  (((a) >> 24) == 127)

/* --- the interface -------------------------------------------------- */

struct netif {
    struct netdev *dev;
    u8   mac[ETH_ALEN];
    ip4_t ip;
    ip4_t netmask;
    ip4_t gateway;
    int  up;

    /* Counters, because the first question about a network that is not
     * working is always which direction stopped. */
    u32  rx_packets;
    u32  tx_packets;
    u32  rx_dropped;
    u32  tx_errors;
};

/* The network card's interface, eth0, and the loopback, lo. lo exists
 * whether or not there is a card: 127.0.0.1/8, up until taken down. */
struct netif *net_if(void);
struct netif *net_lo(void);

/*
 * Bring the interface up on whatever netdev registered itself. Returns
 * -ENODEV if there is no network hardware, which is not fatal: a machine
 * with no NIC should still get a prompt.
 */
int  net_init(void);

/*
 * Take whatever has arrived and dispatch it.
 *
 * CALLED FROM ORDINARY KERNEL CONTEXT, never from an interrupt handler.
 * That is a deliberate restriction and the reason the whole stack needs
 * no locking: there is one place protocol code runs, and it is not
 * reentrant with itself. The driver's interrupt, when there is one, will
 * do nothing but move frames into a ring for this to drain.
 *
 * Returns how many frames it handled.
 */
int  net_poll(void);

/* Queue a frame for delivery to this machine, as loopback. From task
 * context only; net_poll() delivers it. */
int  net_loopback(const void *frame, u32 len);

/* The body of netd, the kernel task that keeps the stack moving. */
void net_task(void);

/*
 * Take frames off the card and into memory, and nothing else.
 *
 * CALLED FROM THE TIMER INTERRUPT. It touches the driver and a ring and
 * runs no protocol code, which is what makes it safe there.
 *
 * This is not an optimisation. The LAN91C111 holds arriving frames in a
 * small pool of packet pages that it also allocates TRANSMIT buffers
 * from, so a card whose receiver is never drained stops being able to
 * send -- and on a real network, where broadcasts arrive constantly
 * whether or not anything here is interested, that happens within
 * seconds. Under QEMU's user-mode NAT almost nothing arrives unasked,
 * which is exactly why this was not noticed until the machine was put
 * on a real LAN.
 */
void net_drain(void);

/*
 * Wait for something, while keeping the network running.
 *
 * Everything that blocks here spins, because there is no scheduler to
 * sleep against -- the same spin the console has always done. What makes
 * it work is that this drives net_poll() round the loop, so replies
 * arrive while something waits for them. When there are tasks this
 * becomes a sleep on a wait queue and every caller stays as it is.
 *
 * Returns 1 if `*flag` became non-zero, 0 if it timed out.
 */
int  net_wait(volatile int *flag, u32 ms);

/* Sleep for up to `ms`, waking early if a packet arrives. What every
 * wait in the stack does instead of spinning. */
void net_sleep(u32 ms);

/* Send one complete ethernet frame. */
int  net_tx(const void *frame, u32 len);

/*
 * Build an ethernet header at `buf` and return where the payload goes.
 * A null destination means broadcast.
 */
void *net_eth_hdr(void *buf, const u8 *dst, u16 type);

int  net_is_local(ip4_t addr);      /* on our subnet? */

void net_set_addr(ip4_t ip, ip4_t mask, ip4_t gw);

/* --- what the layers above register ---------------------------------- */

/* --- IP -------------------------------------------------------------- */

#define IPPROTO_ICMP    1
#define IPPROTO_UDP     17
#define IPPROTO_TCP     6

/* from_lo: the frame came off the loopback, not the wire -- the only
 * way 127/8 may arrive. */
void ip_input(const void *frame, u32 len, int from_lo);
int  ip_output(ip4_t dst, u8 proto, const void *payload, u32 len);

/*
 * The internet checksum (RFC 1071). `start` lets a caller fold in a
 * pseudo-header before the data, which is what UDP and TCP need.
 */
u16  net_checksum(const void *data, u32 len, u32 start);

/* --- ICMP ------------------------------------------------------------ */

void icmp_input(ip4_t from, const void *data, u32 len);
int  icmp_ping(ip4_t to, u32 timeout_ms, u32 *rtt_ms);

/* --- UDP ------------------------------------------------------------- */

/*
 * What a bound port is called with. Inside the kernel, not a socket:
 * this is what DHCP needs to exist, and sockets will be built on it
 * once there is something for a program to hold.
 */
typedef void (*udp_handler_t)(void *arg, ip4_t from, u16 sport,
                              const void *data, u32 len);

void tcp_input(ip4_t src, ip4_t dst, const void *seg, u32 len);

void udp_input(ip4_t from, ip4_t to, const void *data, u32 len);
int  udp_output(ip4_t dst, u16 dport, u16 sport, const void *data, u32 len);
int  udp_bind(u16 port, udp_handler_t fn, void *arg);
void udp_unbind(u16 port);
int  udp_port_in_use(u16 port);

/* --- DHCP ------------------------------------------------------------ */

/* Run the exchange and configure the interface. Returns 0 or -errno. */
int   dhcp_configure(void);
u32   dhcp_lease_seconds(void);
ip4_t dhcp_server(void);
ip4_t dhcp_dns(void);

/* --- sockets --------------------------------------------------------- */

struct sockaddr_in;

/* Sockets. The system call layer copies addresses; these take kernel
 * pointers and the open file. See socket.c. */
struct file;
int  sock_create(int domain, int type, int protocol);
int  sock_is(struct file *f);
int  sock_dgram(struct file *f);   /* a UDP socket? */
int  sock_bind(struct file *f, const struct sockaddr_in *sa);
int  sock_connect(struct file *f, const struct sockaddr_in *sa);
int  sock_listen(struct file *f, int backlog);
int  sock_accept(struct file *f, struct sockaddr_in *peer, int flags);
s32  sock_send(struct file *f, const void *buf, u32 len, int flags,
               const struct sockaddr_in *to);
s32  sock_recv(struct file *f, void *buf, u32 len, int flags,
               struct sockaddr_in *from, int *truncated);
int  sock_shutdown(struct file *f, int how);
int  sock_name(struct file *f, struct sockaddr_in *sa, int peer);
int  sock_setopt(struct file *f, int level, int name, const void *val,
                 u32 len);
int  sock_getopt(struct file *f, int level, int name, void *val, u32 *len);

void arp_init(void);
void arp_input(const void *frame, u32 len);

/*
 * Look up the hardware address for an IP, sending a request and waiting
 * if it is not known. Returns 0 and fills `mac`, or -errno.
 */
int  arp_resolve(ip4_t addr, u8 *mac);

/* Just send the request, for `arping`. */
int  arp_request(ip4_t addr);

/* Walk the cache, for `arp`. Returns 0 past the end. */
int  arp_entry(int index, ip4_t *addr, u8 *mac, u32 *age_ms);

#endif /* NET_H */
