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
 * The state machine of RFC 793, both opens, and an orderly close on
 * both sides. Round trip measurement and a computed retransmission
 * timeout (RFC 6298) with Karn's algorithm. Slow start, congestion
 * avoidance, fast retransmit and fast recovery (RFC 5681). A
 * reassembly queue for segments that arrive ahead of a gap. Delayed
 * acknowledgements. Initial sequence numbers that an off-path
 * attacker cannot guess (RFC 6528).
 *
 * Most of that list was once a list of things this file deliberately
 * did NOT do, on the grounds that a machine talking to its own LAN is
 * not where the internet's congestion is decided. That reasoning held
 * exactly as long as the only network was QEMU's NAT. The moment the
 * interface was bridged onto a real one the omissions became defects:
 * without reassembly one lost packet stalls a transfer for a whole
 * round trip, and an ISN taken from the tick is guessable by anyone
 * who knows roughly when the connection was made.
 *
 * WHAT IT STILL DOES NOT, each a decision rather than an oversight:
 *
 *   No window scaling, no SACK, no timestamps. All are options, all
 *   are negotiated, and a peer that offers them works perfectly well
 *   with a stack that declines. Window scaling would matter on a path
 *   whose bandwidth-delay product exceeds 64 KB; this machine's
 *   receive buffer is 4 KB, so the window is the binding constraint
 *   long before the field width is.
 *
 *   No PAWS, which needs timestamps.
 *
 *   No path MTU discovery. The MSS is fixed at what fits an ethernet
 *   frame, and everything this machine can reach is an ethernet hop
 *   or behind a router that will fragment.
 *
 *   No Nagle. Small writes go out as they are made. A machine with a
 *   4 KB send buffer and a human at the other end of it is not where
 *   the forty-byte-header problem is solved, and coalescing would
 *   make an interactive session worse.
 *
 * The socket layer above this file is what keeps the option open: a
 * program calls socket(), connect() and read(), and which
 * implementation answers is not its business.
 *
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

/*
 * The buffers, from the page allocator per connection. Powers of two,
 * because both are rings. The receive buffer is what the window can
 * say, and 128 KB is past what 16 bits of window can: that is what
 * window scaling is for (TCP_WSCALE below).
 */
#define TCP_SNDBUF      (64 * 1024)
#define TCP_RCVBUF      (128 * 1024)
#define TCP_MAX_CONNS   16

/* Our window shift: 128 KB / 4 fits the 16-bit field. RFC 7323. */
#define TCP_WSCALE      2

/* SACK blocks remembered from the peer (the sender's scoreboard). */
#define TCP_SACK_MAX    8

/*
 * Segments held while a gap in front of them is filled.
 *
 * Out-of-order data used to be dropped, which is legal and turns one
 * lost packet into a stall for a whole round trip: the sender has to
 * time out before anything else can be accepted. Holding a few
 * segments means the retransmission of the ONE missing piece completes
 * the run and everything queued behind it is delivered at once.
 *
 * The buffers are a shared pool rather than per connection, because a
 * reassembly queue is only occupied during a loss -- giving every
 * connection its own would reserve memory for a situation that is rare
 * on all of them at once.
 */
#define TCP_OOO_SLOTS   64
#define TCP_OOO_PER_CB  24

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

    /*
     * Congestion control, RFC 5681. The peer's window says what it can
     * RECEIVE; the congestion window is this end's guess at what the
     * path between them can carry. A sender may use the smaller of the
     * two and nothing else -- which is the whole idea, and the reason a
     * stack without it can make a congested link worse.
     */
    u32   cwnd;
    u32   ssthresh;             /* where slow start gives way        */
    u32   dupacks;              /* consecutive duplicate ACKs        */
    u32   recover;              /* snd_nxt when fast recovery began  */
    int   in_recovery;

    /*
     * Round trip time, RFC 6298. The retransmission timeout used to be
     * a constant that doubled; measuring it means recovering from a
     * loss in something close to one round trip rather than half a
     * second.
     */
    u32   srtt_ms;              /* smoothed round trip               */
    u32   rttvar_ms;            /* its variation                     */
    u32   rtt_seq;              /* the sequence being timed          */
    u32   rtt_start;            /* jiffies when it went out          */
    int   rtt_timing;
    int   rtt_valid;            /* srtt holds a real measurement     */

    /* Receive sequence space. */
    u32   rcv_nxt;              /* next expected                    */

    /* Rings, allocated with the connection (tcp_alloc). The send ring
     * holds sndlen bytes starting at sndstart, which is snd_una. */
    u8   *sndbuf;
    u32   sndstart;
    u32   sndlen;               /* bytes held, starting at snd_una  */
    u32   snd_max;              /* the highest sequence ever sent   */

    u8   *rcvbuf;
    u32   rcvhead;
    u32   rcvtail;

    /*
     * The options negotiated in the SYN exchange (RFC 7323, RFC 2018).
     * Each is on only if both ends offered it.
     */
    int   ws_ok;                /* window scaling                   */
    u8    snd_wscale;           /* the peer's shift, for its window */
    u8    rcv_wscale;           /* ours, for the window we send     */
    int   ts_ok;                /* timestamps                       */
    u32   ts_recent;            /* the peer's latest TSval, echoed  */
    int   sack_ok;              /* selective acknowledgements       */
    int   peer_ws, peer_ts, peer_sack;  /* what the peer's SYN offered */

    /* The sender's SACK scoreboard: ranges the peer says it holds,
     * above snd_una, which a retransmission need not send again. */
    struct {
        u32 start, end;
    } sacked[TCP_SACK_MAX];
    int   nsacked;
    u32   high_rxt;             /* retransmitted up to here, in recovery */

    /* Keepalive (RFC 1122 4.2.3.6), with Linux's knobs. */
    int   keepalive;
    u32   keep_idle_s, keep_intvl_s, keep_cnt;
    u32   last_rcv;             /* jiffies of the last segment in   */
    u32   keep_next;            /* jiffies of the next probe        */
    u32   keep_probes;          /* sent since the peer last spoke   */

    /* Counters, for netstat and for the tests that judge the above. */
    u32   rexmit_segs, rexmit_bytes;
    u32   max_snd_wnd;          /* the largest window the peer gave */
    u32   keep_sent;

    /* Retransmission. */
    u32   rto_ms;
    u32   rexmit_at;            /* jiffies, 0 when nothing is out   */
    int   rexmits;

    u32   timewait_at;

    /* Out-of-order segments, waiting for the gap in front of them. */
    struct {
        int used;
        int slot;               /* index into the shared buffer pool  */
        u32 seq;
        u32 len;
        u32 when;               /* arrival order, for the SACK report */
    } ooo[TCP_OOO_PER_CB];

    /* Delayed acknowledgement: how many segments have gone unanswered
     * and when the first of them arrived. */
    int   unacked_segs;
    u32   delack_at;

    int   fin_sent;
    int   fin_pending;          /* close() asked; sent after the data */
    int   fin_rcvd;
    int   reset;                /* the peer aborted it              */
    int   timed_out;            /* retransmission gave up           */

    /*
     * Closed by its owner and left to finish on its own: the FIN, the
     * wait for the peer's, TIME_WAIT. netd runs the timers, and
     * tcp_timer frees it when it gets to CLOSED -- or gives up on a peer
     * that never finishes.
     */
    int   orphan;
    u32   orphan_since;

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

/* Like tcp_recv, but leaves the data where it is (MSG_PEEK). */
s32  tcp_peek(struct tcpcb *t, void *data, u32 len);

/* Is any connection using this local port -- TIME_WAIT included? */
int  tcp_port_in_use(u16 port);

/* A free ephemeral port, or 0. */
u16  tcp_pick_port(void);

/*
 * The owner is done with it: close it, and let it finish on its own.
 * A listener's unaccepted connections are closed with it. After this
 * the caller must not touch `t` again.
 */
void tcp_release(struct tcpcb *t);

/* POLLIN / POLLOUT / POLLERR / POLLHUP for this connection, now. */
int  tcp_poll(struct tcpcb *t);

/*
 * Test and comparison knobs, set with netctl: drop every Nth data
 * segment sent over loopback (0 for none), and turn off the options
 * that would otherwise be offered in a SYN.
 */
void tcp_set_loss(u32 every);
u32  tcp_loss_every(void);
void tcp_set_disabled(u32 mask);     /* TCPOPT_NO_* in uapi.h */

/* Walk the connection table, for netstat. Returns 0 past the end. */
struct tcpcb *tcp_nth(int index);
const char *tcp_state_name(int state);

#endif /* TCP_H */
