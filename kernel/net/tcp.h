/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tcp.h - a connection, and what can be done to one.
 *
 * WHY THIS IS WRITTEN OUT RATHER THAN IMPORTED. The design notes say to
 * bring lwIP in at TCP, and that was the right call when the layers
 * below it did not exist. They do now: ARP, IP, ICMP and UDP are here,
 * documented, and wired into the device model and the shell. lwIP is not
 * a TCP -- it is a whole stack, with its own ARP, its own IP and its own
 * idea of what an interface is -- so adopting it means discarding all of
 * that, not slotting a layer in on top. The socket layer above this file
 * is what keeps the option open: a program calls socket(), connect() and
 * read(), and which implementation answers is not its business.
 *
 * WHAT THIS IMPLEMENTS
 *
 * The state machine of RFC 793, both opens, retransmission with a
 * backed-off timer, and an orderly close on both sides. Enough to fetch
 * a page from a real web server and enough to be one.
 *
 * WHAT IT DOES NOT, each a decision rather than an oversight:
 *
 *   No congestion control. No slow start, no congestion window, no fast
 *   retransmit. A machine that talks to things on its own LAN is not
 *   where the internet's congestion is decided, and the algorithms are
 *   where TCP stops being a protocol and starts being a research field.
 *   The send window is whatever the peer advertised, capped by the send
 *   buffer.
 *
 *   No out-of-order reassembly. A segment that arrives ahead of a gap is
 *   dropped and the sender retransmits it. That IS legal -- a receiver
 *   is permitted to drop anything it does not want -- and it costs
 *   throughput on a lossy path rather than correctness. A reassembly
 *   queue is the single biggest piece of TCP and it earns nothing on a
 *   LAN that does not lose packets.
 *
 *   No window scaling, no SACK, no timestamps. All are options, all are
 *   negotiated, and a peer that offers them works perfectly well with a
 *   stack that declines.
 *
 *   No PAWS and no random initial sequence numbers. The ISN comes from
 *   the tick. On a machine that is not on the open internet that is a
 *   theoretical exposure; on one that is, it is a real one, and this is
 *   the sentence to come back to.
 */
#ifndef TCP_H
#define TCP_H

#include "net.h"

/* RFC 793's names, so that the state machine can be read against it. */
enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

#define TCP_SNDBUF      2048
#define TCP_RCVBUF      2048
#define TCP_MAX_CONNS   8

struct tcpcb {
    int   used;
    int   state;

    ip4_t local_ip;
    u16   local_port;
    ip4_t remote_ip;
    u16   remote_port;

    /* Send sequence space, RFC 793 section 3.2. */
    u32   snd_una;              /* oldest unacknowledged            */
    u32   snd_nxt;              /* next to send                     */
    u32   snd_wnd;              /* what the peer will accept        */

    /* Receive sequence space. */
    u32   rcv_nxt;              /* next expected                    */

    u8    sndbuf[TCP_SNDBUF];
    u32   sndlen;               /* bytes held, starting at snd_una  */

    u8    rcvbuf[TCP_RCVBUF];
    u32   rcvhead;
    u32   rcvtail;

    /* Retransmission. */
    u32   rto_ms;
    u32   rexmit_at;            /* jiffies, 0 when nothing is out   */
    int   rexmits;

    u32   timewait_at;

    int   fin_sent;
    int   fin_rcvd;
    int   reset;                /* the peer aborted it              */

    /* A listening socket parks accepted connections here. */
    struct tcpcb *listener;
    int   pending;              /* finished its handshake, unaccepted */
};

void tcp_init(void);
void tcp_input(ip4_t src, ip4_t dst, const void *seg, u32 len);

/*
 * Timers. Called from net_poll(), so in ordinary kernel context rather
 * than from the interrupt -- retransmission builds and sends segments,
 * which is protocol work like any other.
 */
void tcp_timer(void);

struct tcpcb *tcp_alloc(void);
void tcp_free(struct tcpcb *t);

int  tcp_connect(struct tcpcb *t, ip4_t addr, u16 port);
int  tcp_listen(struct tcpcb *t, u16 port);
struct tcpcb *tcp_accept(struct tcpcb *t);
s32  tcp_send(struct tcpcb *t, const void *data, u32 len);
s32  tcp_recv(struct tcpcb *t, void *data, u32 len);
int  tcp_close(struct tcpcb *t);
u32  tcp_available(struct tcpcb *t);

#endif /* TCP_H */
