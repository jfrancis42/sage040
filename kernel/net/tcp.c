/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tcp.c - the state machine, and a window.
 *
 * Reference: RFC 793, with RFC 1122's corrections.
 *
 * SEQUENCE NUMBERS WRAP, and every comparison here has to survive it.
 * A 32-bit sequence space wraps after 4 GB, which on a fast link is
 * minutes -- so `a < b` is never written, and seq_lt() subtracts and
 * looks at the sign instead. Getting this wrong produces a connection
 * that works perfectly for hours and then hangs, which is the hardest
 * kind of bug to be handed.
 */
#include "tcp.h"
#include "random.h"
#include "timer.h"
#include "console.h"
#include "errno.h"
#include "string.h"

#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10

struct tcphdr {
    u16 sport;
    u16 dport;
    u32 seq;
    u32 ack;
    u8  offset;                 /* header length in 32-bit words, top nibble */
    u8  flags;
    u16 window;
    u16 check;
    u16 urgent;
} PACKED;

#define TCP_HDR_LEN ((int)sizeof(struct tcphdr))
#define TCP_MSS     1400        /* comfortably inside an ethernet frame */

/*
 * RFC 6298 puts the minimum at one second, which is right for a path
 * that might cross an ocean and far too slow for one that does not
 * leave the building. 200 ms is the value most systems actually use.
 */
#define RTO_MIN_MS      200
#define RTO_MAX_MS      8000
#define RTO_INITIAL_MS  1000    /* before anything has been measured */
#define REXMIT_LIMIT    6
#define TIMEWAIT_MS     10000   /* not 2MSL; see tcp_timer */

/* A delayed acknowledgement waits this long, or until a second segment
 * arrives -- whichever comes first. RFC 1122 allows 500 ms and requires
 * an ACK for every second full-sized segment. */
#define DELACK_MS       200

#define IW              (2 * TCP_MSS)   /* the initial congestion window */

/*
 * Buffers for segments held out of order. Shared, because a reassembly
 * queue is only occupied while a connection is recovering from a loss.
 */
static u8  ooo_pool[TCP_OOO_SLOTS][TCP_MSS];
static int ooo_used[TCP_OOO_SLOTS];

static struct tcpcb conns[TCP_MAX_CONNS];

static void ooo_flush(struct tcpcb *t);
static void send_data(struct tcpcb *t);
static int  send_seg(struct tcpcb *t, u32 seq, u8 flags,
                     const void *data, u32 len);
static u16 next_port = 32768;

/*
 * An initial sequence number.
 *
 * This was the clock multiplied by a prime, which is to say it was
 * guessable by anybody who knew roughly when the connection was made --
 * and an off-path attacker who can guess an ISN can inject data into a
 * connection they cannot see. RFC 6528 describes the shape of the
 * answer: something unpredictable, advancing with time so that an old
 * duplicate from a previous connection on the same port pair cannot be
 * mistaken for current data.
 *
 * The random half comes from random.c, which is not cryptographic and
 * says so; the time half is the tick. Between them an off-path guess
 * has to be right about both.
 */
static u32 tcp_isn(void)
{
    return random_u32() + timer_jiffies() * 64;
}

/* --- sequence arithmetic -------------------------------------------- */

static int seq_le(u32 a, u32 b)  { return (s32)(a - b) <= 0; }
static int seq_ge(u32 a, u32 b)  { return (s32)(a - b) >= 0; }
static int seq_gt(u32 a, u32 b)  { return (s32)(a - b) > 0; }

/* --- the table ------------------------------------------------------ */

struct tcpcb *tcp_nth(int index)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].used && index-- == 0) {
            return &conns[i];
        }
    }
    return 0;
}

const char *tcp_state_name(int state)
{
    switch (state) {
    case TCP_LISTEN:       return "LISTEN";
    case TCP_SYN_SENT:     return "SYN_SENT";
    case TCP_SYN_RECEIVED: return "SYN_RCVD";
    case TCP_ESTABLISHED:  return "ESTABLISHED";
    case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
    case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_CLOSING:      return "CLOSING";
    case TCP_LAST_ACK:     return "LAST_ACK";
    case TCP_TIME_WAIT:    return "TIME_WAIT";
    default:               return "CLOSED";
    }
}

void tcp_init(void)
{
    memset(conns, 0, sizeof(conns));
}

struct tcpcb *tcp_alloc(void)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].used) {
            memset(&conns[i], 0, sizeof(conns[i]));
            conns[i].used = 1;
            conns[i].state = TCP_CLOSED;
            conns[i].rto_ms = RTO_INITIAL_MS;
            conns[i].cwnd = IW;
            conns[i].ssthresh = 64 * 1024;  /* effectively no limit yet */
            return &conns[i];
        }
    }
    return 0;
}

void tcp_free(struct tcpcb *t)
{
    if (t) {
        ooo_flush(t);           /* give the shared buffers back */
        t->used = 0;
        t->state = TCP_CLOSED;
    }
}

static struct tcpcb *find(ip4_t raddr, u16 rport, u16 lport)
{
    int i;

    /* An established connection wins over a listener on the same port,
     * which is what lets a server keep accepting while it talks. */
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcpcb *t = &conns[i];

        if (t->used && t->state != TCP_LISTEN &&
            t->local_port == lport &&
            t->remote_port == rport && t->remote_ip == raddr) {
            return t;
        }
    }
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcpcb *t = &conns[i];

        if (t->used && t->state == TCP_LISTEN && t->local_port == lport) {
            return t;
        }
    }
    return 0;
}

/* --- the receive buffer --------------------------------------------- */

static u32 rcv_used(struct tcpcb *t)
{
    return (t->rcvhead - t->rcvtail) % TCP_RCVBUF;
}

static u32 rcv_free(struct tcpcb *t)
{
    return TCP_RCVBUF - 1 - rcv_used(t);
}

static void rcv_put(struct tcpcb *t, const u8 *p, u32 n)
{
    while (n-- > 0 && rcv_free(t) > 0) {
        t->rcvbuf[t->rcvhead] = *p++;
        t->rcvhead = (t->rcvhead + 1) % TCP_RCVBUF;
    }
}

u32 tcp_available(struct tcpcb *t)
{
    return rcv_used(t);
}

/* --- round trip time, RFC 6298 --------------------------------------- */

/*
 * One measurement, folded into the estimate.
 *
 * The constants are the standard's: the smoothed value moves an eighth
 * of the way towards each sample, and the variation a quarter of the
 * way towards each deviation. Working in eighths and quarters means the
 * whole thing is shifts, which matters on a machine with no divider
 * worth using.
 *
 * KARN'S ALGORITHM is why `rtt_timing` exists. A segment that was
 * retransmitted cannot be measured: when the acknowledgement arrives
 * there is no way to know which copy it answers, and assuming the
 * second gives a measurement far too short -- which shortens the
 * timeout, which causes more retransmissions. So a timed segment that
 * has to be sent again simply stops being timed.
 */
static void rtt_update(struct tcpcb *t, u32 measured_ms)
{
    if (!t->rtt_valid) {
        t->srtt_ms = measured_ms;
        t->rttvar_ms = measured_ms / 2;
        t->rtt_valid = 1;
    } else {
        u32 delta = (t->srtt_ms > measured_ms)
                  ? t->srtt_ms - measured_ms
                  : measured_ms - t->srtt_ms;

        t->rttvar_ms = (3 * t->rttvar_ms + delta) / 4;
        t->srtt_ms = (7 * t->srtt_ms + measured_ms) / 8;
    }

    /* RTO = srtt + 4 * rttvar, which is the standard's way of saying
     * "long enough that ordinary variation does not trip it". */
    t->rto_ms = t->srtt_ms + 4 * t->rttvar_ms;
    if (t->rto_ms < RTO_MIN_MS) {
        t->rto_ms = RTO_MIN_MS;
    }
    if (t->rto_ms > RTO_MAX_MS) {
        t->rto_ms = RTO_MAX_MS;
    }
}

/* --- the congestion window, RFC 5681 --------------------------------- */

/*
 * How much may be in flight: the smaller of what the peer will accept
 * and what the path is believed to carry. Using only the first is what
 * a stack without congestion control does, and it is how one machine
 * makes a congested link worse for everybody on it.
 */
static u32 usable_window(const struct tcpcb *t)
{
    u32 w = t->snd_wnd;

    if (t->cwnd < w) {
        w = t->cwnd;
    }
    return w;
}

/* A new acknowledgement opens the window. */
static void cwnd_on_ack(struct tcpcb *t, u32 acked)
{
    if (t->cwnd < t->ssthresh) {
        /*
         * Slow start, which is not slow -- it doubles every round trip.
         * The name is about where it starts, not how it grows.
         */
        t->cwnd += acked < TCP_MSS ? acked : TCP_MSS;
    } else {
        /*
         * Congestion avoidance: roughly one more segment per round
         * trip, which is the additive increase that pairs with the
         * multiplicative decrease below.
         */
        u32 inc = (TCP_MSS * TCP_MSS) / t->cwnd;

        t->cwnd += inc ? inc : 1;
    }
    if (t->cwnd > 32 * 1024) {
        t->cwnd = 32 * 1024;    /* no window scaling, so no point beyond */
    }
}

/* A loss halves it. */
static void cwnd_on_loss(struct tcpcb *t, int timeout)
{
    u32 flight = t->snd_nxt - t->snd_una;
    u32 half = flight / 2;

    if (half < 2 * TCP_MSS) {
        half = 2 * TCP_MSS;
    }
    t->ssthresh = half;

    if (timeout) {
        /*
         * A timeout means nothing is getting through, so start over
         * from one segment. A triple duplicate ACK means segments ARE
         * arriving -- just not that one -- so the window is only
         * halved.
         */
        t->cwnd = TCP_MSS;
        t->in_recovery = 0;
        t->dupacks = 0;
    }
}

/* --- out-of-order segments ------------------------------------------- */

static int ooo_get_slot(void)
{
    int i;

    for (i = 0; i < TCP_OOO_SLOTS; i++) {
        if (!ooo_used[i]) {
            ooo_used[i] = 1;
            return i;
        }
    }
    return -1;
}

static void ooo_release(struct tcpcb *t, int n)
{
    if (t->ooo[n].used) {
        ooo_used[t->ooo[n].slot] = 0;
        t->ooo[n].used = 0;
    }
}

static void ooo_flush(struct tcpcb *t)
{
    int i;

    for (i = 0; i < TCP_OOO_PER_CB; i++) {
        ooo_release(t, i);
    }
}

/*
 * Hold a segment that arrived ahead of a gap.
 *
 * Dropped instead if there is nowhere to put it, which is exactly what
 * the stack used to do with everything -- so the worst case is the old
 * behaviour and anything better is a gain.
 */
static void ooo_queue(struct tcpcb *t, u32 seq, const u8 *data, u32 len)
{
    int i, slot;

    if (len == 0 || len > TCP_MSS) {
        return;
    }
    /* Already held? A retransmission of something queued is common. */
    for (i = 0; i < TCP_OOO_PER_CB; i++) {
        if (t->ooo[i].used && t->ooo[i].seq == seq) {
            return;
        }
    }
    for (i = 0; i < TCP_OOO_PER_CB; i++) {
        if (!t->ooo[i].used) {
            break;
        }
    }
    if (i == TCP_OOO_PER_CB) {
        return;
    }
    slot = ooo_get_slot();
    if (slot < 0) {
        return;
    }
    memcpy(ooo_pool[slot], data, len);
    t->ooo[i].used = 1;
    t->ooo[i].slot = slot;
    t->ooo[i].seq = seq;
    t->ooo[i].len = len;
}

/* --- sending -------------------------------------------------------- */

static u32 pseudo_sum(ip4_t src, ip4_t dst, u32 len)
{
    return (src >> 16) + (src & 0xffff) +
           (dst >> 16) + (dst & 0xffff) +
           IPPROTO_TCP + len;
}

static int send_seg(struct tcpcb *t, u32 seq, u8 flags,
                    const void *data, u32 len)
{
    u8 buf[TCP_HDR_LEN + TCP_MSS + 4];
    struct tcphdr *h = (struct tcphdr *)buf;
    u32 total = (u32)TCP_HDR_LEN + len;
    u8 *opt = buf + TCP_HDR_LEN;

    if (len > TCP_MSS) {
        return -EMSGSIZE;
    }

    memset(buf, 0, TCP_HDR_LEN);
    h->sport = t->local_port;
    h->dport = t->remote_port;
    h->seq = seq;
    h->ack = t->rcv_nxt;
    h->flags = flags;
    h->window = (u16)rcv_free(t);
    h->urgent = 0;

    /*
     * A SYN carries the maximum segment size, because the default of 536
     * is what a peer assumes otherwise -- and a server that believes it
     * is talking over a 536-byte path sends three segments where one
     * would do.
     */
    if (flags & TCP_SYN) {
        opt[0] = 2;             /* kind: MSS   */
        opt[1] = 4;             /* length      */
        opt[2] = (u8)(TCP_MSS >> 8);
        opt[3] = (u8)TCP_MSS;
        h->offset = (u8)(((TCP_HDR_LEN + 4) / 4) << 4);
        memcpy(buf + TCP_HDR_LEN + 4, data, len);
        total += 4;
    } else {
        h->offset = (u8)((TCP_HDR_LEN / 4) << 4);
        memcpy(opt, data, len);
    }

    h->check = 0;
    h->check = net_checksum(buf, total,
                            pseudo_sum(t->local_ip, t->remote_ip, total));

    return ip_output(t->remote_ip, IPPROTO_TCP, buf, total);
}

/*
 * Send whatever the window allows that has not been sent.
 *
 * Nothing is removed from the send buffer here -- it is held until the
 * peer acknowledges it, because until then it may have to go again.
 * That is the whole of what makes TCP reliable and it is why the buffer
 * is indexed from snd_una rather than consumed.
 */
static void send_data(struct tcpcb *t)
{
    u32 inflight, window, offset, n;

    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) {
        return;
    }

    for (;;) {
        inflight = t->snd_nxt - t->snd_una;
        if (inflight >= t->sndlen) {
            break;                      /* everything held is out */
        }
        window = usable_window(t);
        if (window > t->sndlen) {
            window = t->sndlen;
        }
        if (inflight >= window) {
            break;                      /* the peer's window is full */
        }

        offset = inflight;
        n = window - inflight;
        if (n > TCP_MSS) {
            n = TCP_MSS;
        }

        if (send_seg(t, t->snd_nxt, TCP_ACK | TCP_PSH,
                     t->sndbuf + offset, n) < 0) {
            break;
        }
        /*
         * Time one segment at a time. More would need a timestamp in
         * every segment, which is the option this stack declines.
         */
        if (!t->rtt_timing) {
            t->rtt_timing = 1;
            t->rtt_seq = t->snd_nxt + n;
            t->rtt_start = timer_jiffies();
        }

        t->snd_nxt += n;
        if (!t->rexmit_at) {
            t->rexmit_at = timer_jiffies() + (t->rto_ms * HZ) / 1000;
        }
    }
}

static void send_ack(struct tcpcb *t)
{
    send_seg(t, t->snd_nxt, TCP_ACK, 0, 0);
}

/*
 * A reset, which is the only segment sent on behalf of a connection that
 * does not exist. It is how a machine says "nothing is listening there"
 * without an ICMP message that would also tell a scanner what is.
 */
static void send_rst(ip4_t dst, const struct tcphdr *in, u32 seglen)
{
    u8 buf[TCP_HDR_LEN];
    struct tcphdr *h = (struct tcphdr *)buf;
    struct netif *n = net_if();

    if (in->flags & TCP_RST) {
        return;                 /* never answer a reset with a reset */
    }

    memset(buf, 0, sizeof(buf));
    h->sport = in->dport;
    h->dport = in->sport;
    h->offset = (u8)((TCP_HDR_LEN / 4) << 4);

    if (in->flags & TCP_ACK) {
        h->seq = in->ack;
        h->flags = TCP_RST;
    } else {
        h->seq = 0;
        h->ack = in->seq + seglen +
                 ((in->flags & TCP_SYN) ? 1 : 0) +
                 ((in->flags & TCP_FIN) ? 1 : 0);
        h->flags = TCP_RST | TCP_ACK;
    }

    h->check = 0;
    h->check = net_checksum(buf, sizeof(buf),
                            pseudo_sum(n->ip, dst, sizeof(buf)));
    ip_output(dst, IPPROTO_TCP, buf, sizeof(buf));
}

/* --- receiving ------------------------------------------------------ */

static void parse_mss(struct tcpcb *t, const u8 *opt, u32 len)
{
    u32 i = 0;

    (void)t;
    while (i < len) {
        u8 kind = opt[i];

        if (kind == 0) {
            return;             /* end of options */
        }
        if (kind == 1) {
            i++;                /* no-op padding */
            continue;
        }
        if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) {
            return;             /* malformed; stop rather than guess */
        }
        /* MSS is read and deliberately not used: TCP_MSS is already
         * below any path this machine will see, and honouring a
         * smaller one needs segmentation logic that earns nothing
         * here. Parsed so that the option block is walked correctly. */
        i += opt[i + 1];
    }
}

static void ack_sent_data(struct tcpcb *t, u32 ack, u32 datalen, u16 window)
{
    u32 acked;

    if (!seq_gt(ack, t->snd_una)) {
        /*
         * A DUPLICATE ACK: same acknowledgement number, no data, and
         * the window unchanged. Three of them mean a segment was lost
         * but the ones after it are arriving -- which is worth acting
         * on immediately rather than waiting for a timeout.
         */
        if (ack == t->snd_una && datalen == 0 && window == t->snd_wnd &&
            t->snd_nxt != t->snd_una) {
            t->dupacks++;

            if (t->dupacks == 3 && !t->in_recovery) {
                /* Fast retransmit: send the one segment the receiver
                 * is asking for, without waiting for the timer. */
                cwnd_on_loss(t, 0);
                t->in_recovery = 1;
                t->recover = t->snd_nxt;
                t->cwnd = t->ssthresh + 3 * TCP_MSS;
                t->rtt_timing = 0;      /* Karn: this one is not timed */
                {
                    u32 n = t->sndlen;

                    if (n > TCP_MSS) {
                        n = TCP_MSS;
                    }
                    if (n > 0) {
                        send_seg(t, t->snd_una, TCP_ACK | TCP_PSH,
                                 t->sndbuf, n);
                    }
                }
            } else if (t->in_recovery) {
                /*
                 * Fast recovery: each further duplicate says another
                 * segment has left the network, so one more may be put
                 * into it.
                 */
                t->cwnd += TCP_MSS;
                send_data(t);
            }
        }
        return;                 /* nothing new acknowledged */
    }

    t->dupacks = 0;

    /*
     * A measurement, if this acknowledges the segment being timed and
     * that segment was never retransmitted (Karn's algorithm).
     */
    if (t->rtt_timing && seq_ge(ack, t->rtt_seq)) {
        u32 ms = (timer_jiffies() - t->rtt_start) * (1000 / HZ);

        rtt_update(t, ms ? ms : 1);
        t->rtt_timing = 0;
    }

    acked = ack - t->snd_una;

    if (t->in_recovery) {
        /* Recovery ends when everything outstanding when it began has
         * been acknowledged. */
        if (seq_ge(ack, t->recover)) {
            t->in_recovery = 0;
            t->cwnd = t->ssthresh;
        }
    } else {
        cwnd_on_ack(t, acked);
    }

    if (acked > t->sndlen) {
        /* Acknowledges our FIN as well as the data. */
        acked = t->sndlen;
    }
    if (acked > 0) {
        memmove(t->sndbuf, t->sndbuf + acked, t->sndlen - acked);
        t->sndlen -= acked;
    }
    t->snd_una = ack;
    t->rexmits = 0;

    /* Stop the timer if everything is acknowledged. */
    t->rexmit_at = (t->snd_una == t->snd_nxt) ? 0
                 : timer_jiffies() + (t->rto_ms * HZ) / 1000;
}

void tcp_input(ip4_t src, ip4_t dst, const void *seg, u32 len)
{
    const struct tcphdr *h = seg;
    struct netif *n = net_if();
    struct tcpcb *t;
    u32 hlen, datalen;
    const u8 *data;

    if (len < (u32)TCP_HDR_LEN) {
        return;
    }
    if (net_checksum(seg, len, pseudo_sum(src, dst, len)) != 0) {
        return;
    }

    hlen = (u32)(h->offset >> 4) * 4;
    if (hlen < (u32)TCP_HDR_LEN || hlen > len) {
        return;
    }
    data = (const u8 *)seg + hlen;
    datalen = len - hlen;

    t = find(src, h->sport, h->dport);
    if (!t) {
        send_rst(src, h, datalen);
        return;
    }

    /* --- a listener meeting a SYN --------------------------------- */
    if (t->state == TCP_LISTEN) {
        struct tcpcb *c;

        if (!(h->flags & TCP_SYN)) {
            send_rst(src, h, datalen);
            return;
        }
        c = tcp_alloc();
        if (!c) {
            send_rst(src, h, datalen);      /* no room; refuse plainly */
            return;
        }
        c->state = TCP_SYN_RECEIVED;
        c->local_ip = n->ip;
        c->local_port = t->local_port;
        c->remote_ip = src;
        c->remote_port = h->sport;
        c->rcv_nxt = h->seq + 1;
        c->snd_una = tcp_isn();
        c->snd_nxt = c->snd_una;
        c->snd_wnd = h->window;
        c->listener = t;
        parse_mss(c, (const u8 *)seg + TCP_HDR_LEN, hlen - TCP_HDR_LEN);

        send_seg(c, c->snd_nxt, TCP_SYN | TCP_ACK, 0, 0);
        c->snd_nxt++;
        c->rexmit_at = timer_jiffies() + (c->rto_ms * HZ) / 1000;
        return;
    }

    /* --- a reset ends it, whatever state it was in ----------------- */
    if (h->flags & TCP_RST) {
        t->reset = 1;
        t->state = TCP_CLOSED;
        return;
    }

    /* --- our SYN being answered ------------------------------------ */
    if (t->state == TCP_SYN_SENT) {
        if (!(h->flags & TCP_SYN)) {
            return;
        }
        if (h->flags & TCP_ACK) {
            if (h->ack != t->snd_nxt) {
                send_rst(src, h, datalen);
                return;
            }
            t->snd_una = h->ack;
            t->rcv_nxt = h->seq + 1;
            t->snd_wnd = h->window;
            t->state = TCP_ESTABLISHED;
            t->rexmit_at = 0;
            parse_mss(t, (const u8 *)seg + TCP_HDR_LEN, hlen - TCP_HDR_LEN);
            send_ack(t);
        } else {
            /* Simultaneous open: both sides sent SYN. Rare, legal. */
            t->rcv_nxt = h->seq + 1;
            t->state = TCP_SYN_RECEIVED;
            send_seg(t, t->snd_una, TCP_SYN | TCP_ACK, 0, 0);
        }
        return;
    }

    /* --- everything else wants an acceptable ACK -------------------- */
    if (!(h->flags & TCP_ACK)) {
        return;
    }
    t->snd_wnd = h->window;

    if (t->state == TCP_SYN_RECEIVED) {
        if (h->ack != t->snd_nxt) {
            return;
        }
        t->snd_una = h->ack;
        t->state = TCP_ESTABLISHED;
        t->rexmit_at = 0;
        if (t->listener) {
            t->pending = 1;     /* accept() can have it now */
        }
    }

    ack_sent_data(t, h->ack, datalen, h->window);

    /* --- data ------------------------------------------------------- */
    if (datalen > 0) {
        if (h->seq == t->rcv_nxt) {
            u32 take = datalen;

            if (take > rcv_free(t)) {
                take = rcv_free(t);
            }
            rcv_put(t, data, take);
            t->rcv_nxt += take;

            /*
             * The gap this segment just filled may have things queued
             * behind it. Draining repeatedly, because each piece can
             * make the next one contiguous in turn -- this is where a
             * single lost segment's retransmission delivers everything
             * that arrived after it.
             */
            for (;;) {
                int i, progressed = 0;

                for (i = 0; i < TCP_OOO_PER_CB; i++) {
                    if (!t->ooo[i].used) {
                        continue;
                    }
                    if (seq_le(t->ooo[i].seq, t->rcv_nxt)) {
                        u32 skip = t->rcv_nxt - t->ooo[i].seq;

                        if (skip < t->ooo[i].len) {
                            u32 n = t->ooo[i].len - skip;

                            if (n > rcv_free(t)) {
                                n = rcv_free(t);
                            }
                            rcv_put(t, ooo_pool[t->ooo[i].slot] + skip, n);
                            t->rcv_nxt += n;
                        }
                        ooo_release(t, i);
                        progressed = 1;
                    }
                }
                if (!progressed) {
                    break;
                }
            }

            /*
             * Delayed acknowledgement. Answering every segment doubles
             * the packet count on a bulk transfer for no gain; RFC 1122
             * allows waiting, as long as every second full segment is
             * answered and nothing waits longer than half a second.
             */
            t->unacked_segs++;
            if (t->unacked_segs >= 2) {
                send_ack(t);
                t->unacked_segs = 0;
                t->delack_at = 0;
            } else if (!t->delack_at) {
                t->delack_at = timer_jiffies() + (DELACK_MS * HZ) / 1000;
            }
        } else if (seq_gt(h->seq, t->rcv_nxt)) {
            /*
             * Ahead of the gap. Held rather than dropped, and
             * acknowledged IMMEDIATELY with the sequence still wanted --
             * that duplicate ACK is what tells the sender which segment
             * to resend, and three of them make it resend without
             * waiting for a timeout.
             */
            ooo_queue(t, h->seq, data, datalen);
            send_ack(t);
            t->unacked_segs = 0;
            t->delack_at = 0;
        } else {
            /* Behind: a retransmission of something already taken. Say
             * where we are, so the sender stops. */
            send_ack(t);
        }
    }

    /* --- the peer closing ------------------------------------------- */
    if ((h->flags & TCP_FIN) && seq_le(h->seq, t->rcv_nxt)) {
        if (!t->fin_rcvd) {
            t->fin_rcvd = 1;
            t->rcv_nxt++;
            send_ack(t);
        }
        switch (t->state) {
        case TCP_ESTABLISHED:
            t->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            t->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2:
            t->state = TCP_TIME_WAIT;
            t->timewait_at = timer_jiffies() + (TIMEWAIT_MS * HZ) / 1000;
            break;
        default:
            break;
        }
    }

    /* --- our own FIN being acknowledged ----------------------------- */
    if (t->fin_sent && t->snd_una == t->snd_nxt) {
        switch (t->state) {
        case TCP_FIN_WAIT_1:
            t->state = t->fin_rcvd ? TCP_TIME_WAIT : TCP_FIN_WAIT_2;
            if (t->state == TCP_TIME_WAIT) {
                t->timewait_at = timer_jiffies() + (TIMEWAIT_MS * HZ) / 1000;
            }
            break;
        case TCP_CLOSING:
            t->state = TCP_TIME_WAIT;
            t->timewait_at = timer_jiffies() + (TIMEWAIT_MS * HZ) / 1000;
            break;
        case TCP_LAST_ACK:
            t->state = TCP_CLOSED;
            break;
        default:
            break;
        }
    }

    send_data(t);
}

/* --- timers --------------------------------------------------------- */

void tcp_timer(void)
{
    u32 now = timer_jiffies();
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcpcb *t = &conns[i];

        if (!t->used) {
            continue;
        }

        /*
         * TIME_WAIT should be twice the maximum segment lifetime, which
         * the standard puts at two minutes. Ten seconds is used instead
         * and it is a real shortcut: it exists to catch a delayed
         * duplicate from this connection being taken as data on the
         * next one with the same port pair, and on a LAN a segment does
         * not survive ten seconds. On a long-haul path it could.
         */
        if (t->state == TCP_TIME_WAIT) {
            if ((s32)(now - t->timewait_at) >= 0) {
                t->state = TCP_CLOSED;
            }
            continue;
        }

        /* A delayed acknowledgement that has waited long enough. */
        if (t->delack_at && (s32)(now - t->delack_at) >= 0) {
            send_ack(t);
            t->unacked_segs = 0;
            t->delack_at = 0;
        }

        if (!t->rexmit_at || (s32)(now - t->rexmit_at) < 0) {
            continue;
        }

        if (++t->rexmits > REXMIT_LIMIT) {
            t->state = TCP_CLOSED;
            t->reset = 1;
            t->rexmit_at = 0;
            continue;
        }

        /*
         * A timeout is the strongest evidence of congestion there is:
         * nothing at all is getting through. The window collapses to
         * one segment and slow start begins again.
         */
        cwnd_on_loss(t, 1);

        /* Karn: whatever was being timed is about to be sent twice, so
         * its acknowledgement can no longer be attributed. */
        t->rtt_timing = 0;

        /*
         * Back off exponentially. A network that dropped the last
         * segment is a network that is likely to drop the next one, and
         * retrying at the same rate is how a stack turns a hiccup into
         * a flood.
         */
        t->rto_ms *= 2;
        if (t->rto_ms > RTO_MAX_MS) {
            t->rto_ms = RTO_MAX_MS;
        }
        t->rexmit_at = now + (t->rto_ms * HZ) / 1000;

        switch (t->state) {
        case TCP_SYN_SENT:
            send_seg(t, t->snd_una, TCP_SYN, 0, 0);
            break;
        case TCP_SYN_RECEIVED:
            send_seg(t, t->snd_una, TCP_SYN | TCP_ACK, 0, 0);
            break;
        default:
            /* Everything unacknowledged, from the start again. */
            t->snd_nxt = t->snd_una;
            if (t->sndlen > 0) {
                send_data(t);
            } else if (t->fin_sent) {
                send_seg(t, t->snd_nxt, TCP_ACK | TCP_FIN, 0, 0);
            }
            break;
        }
    }
}

/* --- what the socket layer calls ------------------------------------ */

static u16 pick_port(void)
{
    int i, tries;

    for (tries = 0; tries < 4096; tries++) {
        u16 p = next_port++;

        if (next_port < 32768) {
            next_port = 32768;  /* wrapped past 65535 */
        }
        for (i = 0; i < TCP_MAX_CONNS; i++) {
            if (conns[i].used && conns[i].local_port == p) {
                break;
            }
        }
        if (i == TCP_MAX_CONNS) {
            return p;
        }
    }
    return 0;
}

int tcp_connect(struct tcpcb *t, ip4_t addr, u16 port)
{
    struct netif *n = net_if();

    if (t->state != TCP_CLOSED) {
        return -EISCONN;
    }
    if (!n->ip) {
        return -EADDRNOTAVAIL;
    }

    t->local_ip = n->ip;
    if (!t->local_port) {
        t->local_port = pick_port();
        if (!t->local_port) {
            return -EADDRINUSE;
        }
    }
    t->remote_ip = addr;
    t->remote_port = port;
    t->snd_una = tcp_isn();
    t->snd_nxt = t->snd_una;
    t->snd_wnd = 1;             /* until the peer says otherwise */
    t->state = TCP_SYN_SENT;

    if (send_seg(t, t->snd_nxt, TCP_SYN, 0, 0) < 0) {
        t->state = TCP_CLOSED;
        return -EIO;
    }
    t->snd_nxt++;
    t->rexmit_at = timer_jiffies() + (t->rto_ms * HZ) / 1000;
    return 0;
}

int tcp_listen(struct tcpcb *t, u16 port)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].used && &conns[i] != t &&
            conns[i].state == TCP_LISTEN && conns[i].local_port == port) {
            return -EADDRINUSE;
        }
    }
    t->local_port = port;
    t->state = TCP_LISTEN;
    return 0;
}

int tcp_poll(struct tcpcb *t)
{
    int r = 0, i;

    if (t->state == TCP_LISTEN) {
        /* A listener is readable when accept() would not wait. */
        for (i = 0; i < TCP_MAX_CONNS; i++) {
            if (conns[i].used && conns[i].listener == t && conns[i].pending) {
                return POLLIN;
            }
        }
        return 0;
    }
    if (t->reset) {
        return POLLIN | POLLERR | POLLHUP;
    }
    /*
     * Readable when there is data, or when there never will be: end of
     * stream is a read that returns 0 at once, and a program waiting in
     * poll() has to be told so it can make it.
     */
    if (tcp_available(t) > 0 || t->fin_rcvd || t->state == TCP_CLOSED) {
        r |= POLLIN;
    }
    if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) &&
        !t->fin_sent && t->sndlen < TCP_SNDBUF) {
        r |= POLLOUT;
    }
    if (t->state == TCP_CLOSED || (t->fin_rcvd && t->fin_sent)) {
        r |= POLLHUP;
    }
    return r;
}

struct tcpcb *tcp_accept(struct tcpcb *t)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].used && conns[i].listener == t && conns[i].pending) {
            conns[i].pending = 0;
            conns[i].listener = 0;
            return &conns[i];
        }
    }
    return 0;
}

s32 tcp_send(struct tcpcb *t, const void *data, u32 len)
{
    u32 room;

    if (t->state == TCP_CLOSED || t->reset) {
        return -ECONNRESET;
    }
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) {
        return -ENOTCONN;
    }
    if (t->fin_sent) {
        return -EPIPE;
    }

    room = TCP_SNDBUF - t->sndlen;
    if (room == 0) {
        return 0;               /* the caller waits and tries again */
    }
    if (len > room) {
        len = room;
    }
    memcpy(t->sndbuf + t->sndlen, data, len);
    t->sndlen += len;

    send_data(t);
    return (s32)len;
}

s32 tcp_recv(struct tcpcb *t, void *data, u32 len)
{
    u8 *out = data;
    u32 n = 0;

    while (n < len && rcv_used(t) > 0) {
        out[n++] = t->rcvbuf[t->rcvtail];
        t->rcvtail = (t->rcvtail + 1) % TCP_RCVBUF;
    }

    if (n > 0) {
        /*
         * The window just opened. Telling the peer immediately is what
         * keeps a transfer moving; waiting for the next segment to
         * carry the news is how a stack stalls at exactly the buffer
         * size.
         */
        send_ack(t);
        return (s32)n;
    }

    if (t->reset) {
        return -ECONNRESET;
    }
    if (t->fin_rcvd) {
        return 0;               /* end of stream, the way read() says it */
    }
    if (t->state == TCP_CLOSED) {
        return -ENOTCONN;
    }
    return -EAGAIN;
}

int tcp_close(struct tcpcb *t)
{
    switch (t->state) {
    case TCP_CLOSED:
    case TCP_LISTEN:
    case TCP_SYN_SENT:
        t->state = TCP_CLOSED;
        tcp_free(t);
        return 0;

    case TCP_ESTABLISHED:
    case TCP_SYN_RECEIVED:
        t->state = TCP_FIN_WAIT_1;
        break;

    case TCP_CLOSE_WAIT:
        t->state = TCP_LAST_ACK;
        break;

    default:
        return 0;               /* already closing */
    }

    send_seg(t, t->snd_nxt, TCP_ACK | TCP_FIN, 0, 0);
    t->snd_nxt++;
    t->fin_sent = 1;
    t->rexmit_at = timer_jiffies() + (t->rto_ms * HZ) / 1000;
    return 0;
}
