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
#include "pmm.h"
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
#define TIMEWAIT_MS     60000   /* 2 MSL, as Linux has it: see tcp_timer */

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
static u32 ooo_clock;           /* arrival order, for SACK's first block */

/* The netctl knobs: see tcp.h. */
static u32 loss_every, loss_count;
static u32 opts_disabled;

void tcp_set_loss(u32 every)
{
    loss_every = every;
    loss_count = 0;
}

u32 tcp_loss_every(void)
{
    return loss_every;
}

void tcp_set_disabled(u32 mask)
{
    opts_disabled = mask;
}

/* The timestamp clock: milliseconds since boot, as RFC 7323 suggests. */
static u32 ts_now(void)
{
    return timer_jiffies() * (1000 / HZ);
}

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
            struct tcpcb *t = &conns[i];
            u32 s = pmm_alloc_pages(TCP_SNDBUF / PAGE_SIZE);
            u32 r = s ? pmm_alloc_pages(TCP_RCVBUF / PAGE_SIZE) : 0;

            if (!r) {
                if (s) {
                    pmm_free_pages(s, TCP_SNDBUF / PAGE_SIZE);
                }
                return 0;
            }
            memset(t, 0, sizeof(*t));
            t->used = 1;
            t->state = TCP_CLOSED;
            t->rto_ms = RTO_INITIAL_MS;
            t->cwnd = IW;
            t->ssthresh = 256 * 1024;   /* effectively no limit yet */
            t->sndbuf = (u8 *)s;
            t->rcvbuf = (u8 *)r;
            /* Linux's defaults: probe after two hours idle, every 75
             * seconds, and give up after nine unanswered. */
            t->keep_idle_s = 7200;
            t->keep_intvl_s = 75;
            t->keep_cnt = 9;
            return t;
        }
    }
    return 0;
}

void tcp_free(struct tcpcb *t)
{
    if (t) {
        ooo_flush(t);           /* give the shared buffers back */
        if (t->sndbuf) {
            pmm_free_pages((u32)t->sndbuf, TCP_SNDBUF / PAGE_SIZE);
            t->sndbuf = 0;
        }
        if (t->rcvbuf) {
            pmm_free_pages((u32)t->rcvbuf, TCP_RCVBUF / PAGE_SIZE);
            t->rcvbuf = 0;
        }
        t->used = 0;
        t->state = TCP_CLOSED;
    }
}

#define ORPHAN_LIMIT_MS 60000   /* how long a closing peer may take */

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
    if (t->cwnd > 256 * 1024) {
        t->cwnd = 256 * 1024;   /* beyond any window this machine has */
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
    t->ooo[i].when = ++ooo_clock;
}

/* --- sending -------------------------------------------------------- */

static u32 pseudo_sum(ip4_t src, ip4_t dst, u32 len)
{
    return (src >> 16) + (src & 0xffff) +
           (dst >> 16) + (dst & 0xffff) +
           IPPROTO_TCP + len;
}

/*
 * The SACK blocks to report: the out-of-order data held, merged into
 * runs, the run holding the most recent arrival first (RFC 2018 says
 * so, because that is the news the sender does not have yet). Returns
 * how many were written, at most `max`.
 */
static int sack_blocks(struct tcpcb *t, u32 out[][2], int max)
{
    u32 blk[TCP_OOO_PER_CB][3];     /* start, end, latest arrival */
    int n = 0, i, j, first = 0;

    for (i = 0; i < TCP_OOO_PER_CB; i++) {
        u32 st, en;
        int merged = 0;

        if (!t->ooo[i].used) {
            continue;
        }
        st = t->ooo[i].seq;
        en = st + t->ooo[i].len;
        for (j = 0; j < n; j++) {
            /* Overlapping or touching: one run. */
            if (seq_le(st, blk[j][1]) && seq_ge(en, blk[j][0])) {
                if (seq_gt(blk[j][0], st)) blk[j][0] = st;
                if (seq_gt(en, blk[j][1])) blk[j][1] = en;
                if (t->ooo[i].when > blk[j][2]) blk[j][2] = t->ooo[i].when;
                merged = 1;
                break;
            }
        }
        if (!merged) {
            blk[n][0] = st;
            blk[n][1] = en;
            blk[n][2] = t->ooo[i].when;
            n++;
        }
    }
    /* A second pass, since merging can make two runs touch. */
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (seq_le(blk[j][0], blk[i][1]) && seq_ge(blk[j][1], blk[i][0])) {
                if (seq_gt(blk[i][0], blk[j][0])) blk[i][0] = blk[j][0];
                if (seq_gt(blk[j][1], blk[i][1])) blk[i][1] = blk[j][1];
                if (blk[j][2] > blk[i][2]) blk[i][2] = blk[j][2];
                blk[j][0] = blk[n - 1][0];
                blk[j][1] = blk[n - 1][1];
                blk[j][2] = blk[n - 1][2];
                n--;
                j = i;
            }
        }
    }
    for (i = 1; i < n; i++) {
        if (blk[i][2] > blk[first][2]) {
            first = i;
        }
    }
    j = 0;
    if (n > 0 && j < max) {
        out[j][0] = blk[first][0];
        out[j][1] = blk[first][1];
        j++;
    }
    for (i = 0; i < n && j < max; i++) {
        if (i != first) {
            out[j][0] = blk[i][0];
            out[j][1] = blk[i][1];
            j++;
        }
    }
    return j;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/*
 * One segment. The options go in here, because every segment carries
 * the same ones once they are agreed:
 *
 *   a SYN offers MSS, SACK-permitted, a timestamp and a window shift
 *   (a SYN-ACK offers only what the peer's SYN did), in Linux's order;
 *   after that, a timestamp on everything if both sides agreed, and on
 *   an ACK with data held out of order, the SACK blocks for it.
 *
 * The window in a SYN is never scaled; in everything after, it is the
 * free space shifted down by the scale this end announced.
 */
static int send_seg(struct tcpcb *t, u32 seq, u8 flags,
                    const void *data, u32 len)
{
    u8 buf[TCP_HDR_LEN + 40 + TCP_MSS];
    struct tcphdr *h = (struct tcphdr *)buf;
    u8 *opt = buf + TCP_HDR_LEN;
    u32 olen = 0, total, win;

    if (len > TCP_MSS) {
        return -EMSGSIZE;
    }

    /*
     * The loss knob, for the tests: every Nth data segment to a
     * loopback address vanishes, as it would on a lossy link -- which
     * the emulator's own network never is.
     */
    if (loss_every && (len > 0 || loss_every == 1) &&
        IP4_IS_LOOPBACK(t->remote_ip) && ++loss_count % loss_every == 0) {
        return 0;               /* 1 drops everything: a dead peer */
    }

    memset(buf, 0, TCP_HDR_LEN);
    h->sport = t->local_port;
    h->dport = t->remote_port;
    h->seq = seq;
    h->ack = t->rcv_nxt;
    h->flags = flags;
    h->urgent = 0;

    if (flags & TCP_SYN) {
        int synack = (flags & TCP_ACK) != 0;
        int ws = !(opts_disabled & TCPOPT_NO_WS) && (!synack || t->peer_ws);
        int ts = !(opts_disabled & TCPOPT_NO_TS) && (!synack || t->peer_ts);
        int sk = !(opts_disabled & TCPOPT_NO_SACK) && (!synack || t->peer_sack);

        /* MSS, because otherwise a peer assumes 536. */
        opt[olen++] = 2;
        opt[olen++] = 4;
        opt[olen++] = (u8)(TCP_MSS >> 8);
        opt[olen++] = (u8)TCP_MSS;
        if (sk) {
            opt[olen++] = 4;            /* SACK permitted */
            opt[olen++] = 2;
        } else if (ts) {
            opt[olen++] = 1;
            opt[olen++] = 1;
        }
        if (ts) {
            opt[olen++] = 8;            /* timestamp */
            opt[olen++] = 10;
            put32(opt + olen, ts_now());
            put32(opt + olen + 4, synack ? t->ts_recent : 0);
            olen += 8;
        }
        if (ws) {
            opt[olen++] = 1;
            opt[olen++] = 3;            /* window scale */
            opt[olen++] = 3;
            opt[olen++] = TCP_WSCALE;
        }
        while (olen % 4) {
            opt[olen++] = 1;
        }
        win = rcv_free(t);
    } else {
        if (t->ts_ok) {
            opt[olen++] = 1;
            opt[olen++] = 1;
            opt[olen++] = 8;
            opt[olen++] = 10;
            put32(opt + olen, ts_now());
            put32(opt + olen + 4, t->ts_recent);
            olen += 8;
        }
        if (t->sack_ok && (flags & TCP_ACK)) {
            u32 blocks[4][2];
            int n = sack_blocks(t, blocks, t->ts_ok ? 3 : 4), k;

            if (n > 0) {
                opt[olen++] = 1;
                opt[olen++] = 1;
                opt[olen++] = 5;
                opt[olen++] = (u8)(2 + 8 * n);
                for (k = 0; k < n; k++) {
                    put32(opt + olen, blocks[k][0]);
                    put32(opt + olen + 4, blocks[k][1]);
                    olen += 8;
                }
            }
        }
        win = rcv_free(t) >> t->rcv_wscale;
    }
    h->window = (u16)(win > 65535 ? 65535 : win);
    h->offset = (u8)(((TCP_HDR_LEN + olen) / 4) << 4);
    memcpy(buf + TCP_HDR_LEN + olen, data, len);
    total = (u32)TCP_HDR_LEN + olen + len;

    h->check = 0;
    h->check = net_checksum(buf, total,
                            pseudo_sum(t->local_ip, t->remote_ip, total));

    return ip_output(t->remote_ip, IPPROTO_TCP, buf, total);
}

/* Copy n bytes from `offset` into the send ring (offset 0 is snd_una). */
static void snd_copy(struct tcpcb *t, u32 offset, u8 *out, u32 n)
{
    u32 at = (t->sndstart + offset) % TCP_SNDBUF;
    u32 first = TCP_SNDBUF - at;

    if (first > n) {
        first = n;
    }
    memcpy(out, t->sndbuf + at, first);
    memcpy(out + first, t->sndbuf, n - first);
}

/* Send n bytes of the ring from sequence `seq`, counting it as a
 * retransmission if it has been sent before. */
static int send_range(struct tcpcb *t, u32 seq, u32 n)
{
    static u8 seg[TCP_MSS];
    int err;

    snd_copy(t, seq - t->snd_una, seg, n);
    err = send_seg(t, seq, TCP_ACK | TCP_PSH, seg, n);
    if (err == 0 && seq_gt(t->snd_max, seq)) {
        t->rexmit_segs++;
        t->rexmit_bytes += n;
    }
    if (err == 0 && seq_gt(seq + n, t->snd_max)) {
        t->snd_max = seq + n;
    }
    return err;
}

/* Is `seq` inside a range the peer has SACKed? Returns the range's end
 * if so, else seq itself. */
static u32 sacked_until(const struct tcpcb *t, u32 seq)
{
    int i;

    for (i = 0; i < t->nsacked; i++) {
        if (seq_ge(seq, t->sacked[i].start) && seq_gt(t->sacked[i].end, seq)) {
            return t->sacked[i].end;
        }
    }
    return seq;
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

    /*
     * Including the states after close(): data queued before it still
     * has to go -- and be retransmitted -- and the FIN goes after it.
     */
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT &&
        t->state != TCP_FIN_WAIT_1 && t->state != TCP_CLOSING &&
        t->state != TCP_LAST_ACK) {
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

        /*
         * Resending after a timeout starts from snd_una again; anything
         * the peer has SACKed on the way is stepped over rather than
         * sent twice. That is the whole of SACK's saving on a timeout.
         */
        {
            u32 skip = sacked_until(t, t->snd_nxt);

            if (skip != t->snd_nxt) {
                if (seq_gt(skip, t->snd_una + t->sndlen)) {
                    skip = t->snd_una + t->sndlen;
                }
                t->snd_nxt = skip;
                continue;
            }
        }

        offset = inflight;
        n = window - inflight;
        if (n > TCP_MSS) {
            n = TCP_MSS;
        }
        /* Not into a SACKed range either. */
        {
            int i;

            for (i = 0; i < t->nsacked; i++) {
                if (seq_gt(t->sacked[i].start, t->snd_nxt) &&
                    seq_gt(t->snd_nxt + n, t->sacked[i].start)) {
                    n = t->sacked[i].start - t->snd_nxt;
                }
            }
        }
        (void)offset;

        if (send_range(t, t->snd_nxt, n) < 0) {
            /*
             * THE FRAME DID NOT GO, AND THE TIMER STILL HAS TO BE
             * ARMED. This used to be a bare `break`, and rexmit_at is
             * set only after a SUCCESSFUL send further down -- so one
             * failed transmit left a connection with data queued, no
             * retransmit timer, and nothing anywhere that would try
             * again. tcp_timer() skips every connection whose
             * rexmit_at is 0, so it skipped this one for ever: the
             * connection sat ESTABLISHED, idle, with its data in the
             * send queue, until something closed it.
             *
             * That is exactly what made the ssh server appear to serve
             * one connection and then stop. The second session's
             * banner -- 1404 bytes -- was queued, the card had no
             * transmit page free at that instant, and the send was
             * never retried. `netstat` on the machine showed
             * ESTABLISHED with Tx-Q 1404, a window of 3314 and a round
             * trip of 10ms: everything ready, nothing moving.
             *
             * A transmit failure is a transient condition -- the card
             * gets its pages back as frames are drained -- so the
             * right response is the same as for a lost segment: wait
             * the retransmit timeout and try again.
             */
            if (!t->rexmit_at) {
                t->rexmit_at = timer_jiffies() + (t->rto_ms * HZ) / 1000;
            }
            break;
        }
        /*
         * Without timestamps, time one segment at a time (with them,
         * every ACK measures -- see ack_sent_data).
         */
        if (!t->ts_ok && !t->rtt_timing) {
            t->rtt_timing = 1;
            t->rtt_seq = t->snd_nxt + n;
            t->rtt_start = timer_jiffies();
        }

        t->snd_nxt += n;
        if (!t->rexmit_at) {
            t->rexmit_at = timer_jiffies() + (t->rto_ms * HZ) / 1000;
        }
    }

    /*
     * THE FIN GOES LAST, once every byte queued before close() is out:
     * it takes the sequence number after the data. It used to be sent
     * the moment close() was called, at snd_nxt, with data still queued
     * behind a full window -- so it took a sequence number the data
     * needed, the peer ended the stream there, and the byte that should
     * have had that number was lost. Found by 100 KB over loopback
     * arriving one byte short.
     *
     * The same test resends it on a retransmission, which has rewound
     * snd_nxt to snd_una: once the data is out again, so is the FIN.
     */
    if ((t->fin_pending || t->fin_sent) &&
        t->snd_nxt - t->snd_una == t->sndlen) {
        if (send_seg(t, t->snd_nxt, TCP_ACK | TCP_FIN, 0, 0) == 0) {
            t->snd_nxt++;
            t->fin_sent = 1;
            t->fin_pending = 0;
            if (!t->rexmit_at) {
                t->rexmit_at = timer_jiffies() + (t->rto_ms * HZ) / 1000;
            }
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
/* `self` is the address the offending segment was sent to, which is who
 * the reset must come from -- 127.x for loopback, not the interface. */
static void send_rst(ip4_t dst, ip4_t self, const struct tcphdr *in,
                     u32 seglen)
{
    u8 buf[TCP_HDR_LEN];
    struct tcphdr *h = (struct tcphdr *)buf;

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
                            pseudo_sum(self, dst, sizeof(buf)));
    ip_output(dst, IPPROTO_TCP, buf, sizeof(buf));
}

/* --- receiving ------------------------------------------------------ */

/* What a segment's options said. */
struct tcpopts {
    int ws;                     /* the peer's shift, or -1           */
    int sack_perm;
    int ts;
    u32 tsval, tsecr;
    int nsack;
    u32 sack[4][2];
};

static u32 get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/*
 * Walk the option block. Bounded by its length as well as by the
 * end-of-options marker, and it stops at anything malformed rather
 * than guessing. MSS is read and not used: TCP_MSS is already below
 * any path this machine will see.
 */
static void parse_opts(const u8 *opt, u32 len, struct tcpopts *o)
{
    u32 i = 0;

    memset(o, 0, sizeof(*o));
    o->ws = -1;
    while (i < len) {
        u8 kind = opt[i], olen;

        if (kind == 0) {
            return;
        }
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) {
            return;
        }
        olen = opt[i + 1];
        switch (kind) {
        case 3:                         /* window scale */
            if (olen == 3) {
                o->ws = opt[i + 2] > 14 ? 14 : opt[i + 2];
            }
            break;
        case 4:                         /* SACK permitted */
            if (olen == 2) {
                o->sack_perm = 1;
            }
            break;
        case 5:                         /* SACK blocks */
            if (olen >= 10 && (olen - 2) % 8 == 0) {
                u32 k, n = (olen - 2) / 8;

                for (k = 0; k < n && k < 4; k++) {
                    o->sack[k][0] = get32(opt + i + 2 + 8 * k);
                    o->sack[k][1] = get32(opt + i + 6 + 8 * k);
                }
                o->nsack = (int)(n < 4 ? n : 4);
            }
            break;
        case 8:                         /* timestamps */
            if (olen == 10) {
                o->ts = 1;
                o->tsval = get32(opt + i + 2);
                o->tsecr = get32(opt + i + 6);
            }
            break;
        default:
            break;
        }
        i += olen;
    }
}

/*
 * What the two SYNs agreed. Each option is on only if the peer offered
 * it and this end did (or, answering, would): RFC 7323 and RFC 2018 are
 * explicit that either side's silence turns it off for both.
 */
static void agree_opts(struct tcpcb *t, const struct tcpopts *o)
{
    t->peer_ws = o->ws >= 0;
    t->peer_ts = o->ts;
    t->peer_sack = o->sack_perm;
    t->ws_ok = t->peer_ws && !(opts_disabled & TCPOPT_NO_WS);
    t->ts_ok = t->peer_ts && !(opts_disabled & TCPOPT_NO_TS);
    t->sack_ok = t->peer_sack && !(opts_disabled & TCPOPT_NO_SACK);
    t->snd_wscale = t->ws_ok ? (u8)o->ws : 0;
    t->rcv_wscale = t->ws_ok ? TCP_WSCALE : 0;
    if (o->ts) {
        t->ts_recent = o->tsval;
    }
}

/* Add a SACKed range to the scoreboard, merging it with what is there. */
static void sack_add(struct tcpcb *t, u32 st, u32 en)
{
    int i;

    if (!seq_gt(en, st) || !seq_gt(en, t->snd_una) || seq_gt(en, t->snd_max)) {
        return;                         /* nonsense, or already acked */
    }
    if (seq_gt(t->snd_una, st)) {
        st = t->snd_una;
    }
    for (i = 0; i < t->nsacked; i++) {
        if (seq_le(st, t->sacked[i].end) && seq_ge(en, t->sacked[i].start)) {
            if (seq_gt(t->sacked[i].start, st)) t->sacked[i].start = st;
            if (seq_gt(en, t->sacked[i].end)) t->sacked[i].end = en;
            return;
        }
    }
    if (t->nsacked < TCP_SACK_MAX) {
        t->sacked[t->nsacked].start = st;
        t->sacked[t->nsacked].end = en;
        t->nsacked++;
    }
}

/* Forget whatever the cumulative ACK has now covered. */
static void sack_trim(struct tcpcb *t)
{
    int i;

    for (i = 0; i < t->nsacked; i++) {
        if (seq_le(t->sacked[i].end, t->snd_una)) {
            t->sacked[i] = t->sacked[--t->nsacked];
            i--;
        } else if (seq_gt(t->snd_una, t->sacked[i].start)) {
            t->sacked[i].start = t->snd_una;
        }
    }
}

/*
 * In recovery with SACK, send the next hole: the first data at or past
 * high_rxt that the peer has not SACKed, below the highest it has. One
 * segment per call -- each duplicate ACK is one more segment's worth of
 * room. Returns 1 if it sent something.
 */
static int sack_rexmit_hole(struct tcpcb *t)
{
    u32 top = t->snd_una, seq, n;
    int i;

    for (i = 0; i < t->nsacked; i++) {
        if (seq_gt(t->sacked[i].end, top)) {
            top = t->sacked[i].end;
        }
    }
    seq = seq_gt(t->high_rxt, t->snd_una) ? t->high_rxt : t->snd_una;
    for (;;) {
        u32 skip = sacked_until(t, seq);

        if (skip == seq) {
            break;
        }
        seq = skip;
    }
    if (!seq_gt(top, seq)) {
        return 0;                       /* no hole below what was SACKed */
    }
    n = top - seq;
    for (i = 0; i < t->nsacked; i++) {
        if (seq_gt(t->sacked[i].start, seq) &&
            seq_gt(seq + n, t->sacked[i].start)) {
            n = t->sacked[i].start - seq;
        }
    }
    if (n > TCP_MSS) {
        n = TCP_MSS;
    }
    if (n == 0 || send_range(t, seq, n) < 0) {
        return 0;
    }
    t->high_rxt = seq + n;
    return 1;
}

static void ack_sent_data(struct tcpcb *t, u32 ack, u32 datalen, u32 window,
                          const struct tcpopts *o)
{
    u32 acked;
    int k;

    /* The peer's SACK blocks go on the scoreboard, whatever else. */
    if (t->sack_ok) {
        for (k = 0; k < o->nsack; k++) {
            sack_add(t, o->sack[k][0], o->sack[k][1]);
        }
    }

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
                t->high_rxt = t->snd_una;
                if (!t->sack_ok || !sack_rexmit_hole(t)) {
                    u32 n = t->sndlen;

                    if (n > TCP_MSS) {
                        n = TCP_MSS;
                    }
                    if (n > 0) {
                        send_range(t, t->snd_una, n);
                        t->high_rxt = t->snd_una + n;
                    }
                }
            } else if (t->in_recovery) {
                /*
                 * Fast recovery: each further duplicate says another
                 * segment has left the network, so one more may be put
                 * into it -- the next hole, if SACK says where one is,
                 * and otherwise new data.
                 */
                t->cwnd += TCP_MSS;
                if (!t->sack_ok || !sack_rexmit_hole(t)) {
                    send_data(t);
                }
            }
        }
        return;                 /* nothing new acknowledged */
    }

    t->dupacks = 0;

    /*
     * A measurement, if this acknowledges the segment being timed and
     * that segment was never retransmitted (Karn's algorithm).
     */
    if (t->ts_ok && o->ts && o->tsecr) {
        /* With timestamps, every ACK of new data is a measurement: the
         * peer echoes when we sent what it is acknowledging. */
        u32 ms = ts_now() - o->tsecr;

        if ((s32)ms >= 0 && ms < 60000) {
            rtt_update(t, ms ? ms : 1);
        }
    } else if (t->rtt_timing && seq_ge(ack, t->rtt_seq)) {
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
        t->sndstart = (t->sndstart + acked) % TCP_SNDBUF;
        t->sndlen -= acked;
    }
    t->snd_una = ack;
    t->rexmits = 0;
    sack_trim(t);

    /* A partial ACK in recovery: the next hole is due now, not at the
     * next duplicate (RFC 6582's point, made with SACK's knowledge). */
    if (t->in_recovery && t->sack_ok) {
        sack_rexmit_hole(t);
    }

    /* Stop the timer if everything is acknowledged. */
    t->rexmit_at = (t->snd_una == t->snd_nxt) ? 0
                 : timer_jiffies() + (t->rto_ms * HZ) / 1000;
}

void tcp_input(ip4_t src, ip4_t dst, const void *seg, u32 len)
{
    const struct tcphdr *h = seg;
    struct tcpcb *t;
    struct tcpopts o;
    u32 hlen, datalen, wnd;
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
        send_rst(src, dst, h, datalen);
        return;
    }
    parse_opts((const u8 *)seg + TCP_HDR_LEN, hlen - TCP_HDR_LEN, &o);

    /* --- a listener meeting a SYN --------------------------------- */
    if (t->state == TCP_LISTEN) {
        struct tcpcb *c;

        if (!(h->flags & TCP_SYN)) {
            send_rst(src, dst, h, datalen);
            return;
        }
        c = tcp_alloc();
        if (!c) {
            send_rst(src, dst, h, datalen);      /* no room; refuse plainly */
            return;
        }
        c->state = TCP_SYN_RECEIVED;
        /* The address the SYN was sent TO: this machine's, or 127.x
         * for loopback -- and the checksums must use the same one. */
        c->local_ip = dst;
        c->local_port = t->local_port;
        c->remote_ip = src;
        c->remote_port = h->sport;
        c->rcv_nxt = h->seq + 1;
        c->snd_una = tcp_isn();
        c->snd_nxt = c->snd_una;
        c->snd_wnd = h->window;         /* a SYN's window is never scaled */
        c->listener = t;
        agree_opts(c, &o);
        /* Keepalive is the listener's, as Linux gives it to accept(). */
        c->keepalive = t->keepalive;
        c->keep_idle_s = t->keep_idle_s;
        c->keep_intvl_s = t->keep_intvl_s;
        c->keep_cnt = t->keep_cnt;
        c->last_rcv = timer_jiffies();

        send_seg(c, c->snd_nxt, TCP_SYN | TCP_ACK, 0, 0);
        c->snd_nxt++;
        c->snd_max = c->snd_nxt;
        c->rexmit_at = timer_jiffies() + (c->rto_ms * HZ) / 1000;
        return;
    }

    /* --- a reset ends it, whatever state it was in ----------------- */
    if (h->flags & TCP_RST) {
        /*
         * Except TIME_WAIT, which a reset must not cut short (RFC 1337):
         * TIME_WAIT exists to outlive stray segments, and an old RST is
         * one of them.
         */
        if (t->state == TCP_TIME_WAIT) {
            return;
        }
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
                send_rst(src, dst, h, datalen);
                return;
            }
            t->snd_una = h->ack;
            t->rcv_nxt = h->seq + 1;
            t->snd_wnd = h->window;     /* unscaled, in a SYN */
            t->max_snd_wnd = t->snd_wnd;
            t->state = TCP_ESTABLISHED;
            t->rexmit_at = 0;
            t->last_rcv = timer_jiffies();
            agree_opts(t, &o);
            send_ack(t);
        } else {
            /* Simultaneous open: both sides sent SYN. Rare, legal. */
            t->rcv_nxt = h->seq + 1;
            t->state = TCP_SYN_RECEIVED;
            agree_opts(t, &o);
            send_seg(t, t->snd_una, TCP_SYN | TCP_ACK, 0, 0);
        }
        return;
    }

    /* --- everything else wants an acceptable ACK -------------------- */
    if (!(h->flags & TCP_ACK)) {
        return;
    }

    /*
     * PAWS (RFC 7323 5): a segment whose timestamp is older than the
     * last one taken is a stray from an earlier trip round the
     * sequence space, and is dropped -- with an ACK, so a peer that
     * merely got out of step learns where things are.
     */
    if (t->ts_ok && o.ts) {
        if (t->ts_recent && (s32)(o.tsval - t->ts_recent) < 0) {
            send_ack(t);
            return;
        }
        if (seq_le(h->seq, t->rcv_nxt)) {
            t->ts_recent = o.tsval;
        }
    }

    /* The peer is alive, whatever this segment is. */
    t->last_rcv = timer_jiffies();
    t->keep_probes = 0;

    wnd = (u32)h->window << t->snd_wscale;
    t->snd_wnd = wnd;
    if (wnd > t->max_snd_wnd) {
        t->max_snd_wnd = wnd;
    }

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

    ack_sent_data(t, h->ack, datalen, wnd, &o);

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
    } else if (seq_gt(t->rcv_nxt, h->seq) &&
               !(h->flags & (TCP_SYN | TCP_FIN))) {
        /*
         * An empty segment from before rcv_nxt is "not acceptable", and
         * RFC 793 says to answer one with an ACK. It is also exactly
         * what a keepalive probe is -- this end's and anybody else's --
         * so without this no probe would ever be answered.
         */
        send_ack(t);
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
        case TCP_TIME_WAIT:
            /* The peer did not get our ACK of its FIN and sent the FIN
             * again: answer it, and wait the full time again. This is
             * the case TIME_WAIT is there for. */
            send_ack(t);
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

        /* An orphan that has finished, or whose peer never will. */
        if (t->orphan && (t->state == TCP_CLOSED ||
                          (s32)(now - t->orphan_since) >=
                          (s32)(ORPHAN_LIMIT_MS * HZ / 1000))) {
            tcp_free(t);
            continue;
        }

        /*
         * TIME_WAIT is twice the maximum segment lifetime, and 60 seconds
         * is what Linux uses for it: long enough that a delayed
         * duplicate from this connection cannot be taken as data on the
         * next one with the same port pair. It was ten seconds, which a
         * segment on a LAN does not survive and one on a long path can.
         */
        if (t->state == TCP_TIME_WAIT) {
            if ((s32)(now - t->timewait_at) >= 0) {
                t->state = TCP_CLOSED;
            }
            continue;
        }

        /*
         * Keepalive. A connection with nothing outstanding and nothing
         * heard for keep_idle_s gets a probe -- a segment one byte
         * behind, which any live peer must answer with an ACK -- every
         * keep_intvl_s, and after keep_cnt unanswered it is declared
         * dead: ETIMEDOUT to whoever next reads it.
         */
        if (t->keepalive && (t->state == TCP_ESTABLISHED ||
                             t->state == TCP_CLOSE_WAIT) &&
            t->snd_una == t->snd_nxt &&
            (s32)(now - t->last_rcv) >= (s32)(t->keep_idle_s * HZ)) {
            if (t->keep_probes == 0 && !t->keep_next) {
                t->keep_next = now;
            }
            if ((s32)(now - t->keep_next) >= 0) {
                if (t->keep_probes >= t->keep_cnt) {
                    t->state = TCP_CLOSED;
                    t->reset = 1;
                    t->timed_out = 1;
                    continue;
                }
                send_seg(t, t->snd_una - 1, TCP_ACK, 0, 0);
                t->keep_probes++;
                t->keep_sent++;
                t->keep_next = now + t->keep_intvl_s * HZ;
            }
        } else {
            t->keep_next = 0;
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
            t->timed_out = 1;       /* ETIMEDOUT, not a refusal */
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
            /* Everything unacknowledged, from the start again -- the
             * data, and then the FIN if one was sent. */
            t->snd_nxt = t->snd_una;
            send_data(t);
            break;
        }
    }
}

/* --- what the socket layer calls ------------------------------------ */

u16 tcp_pick_port(void)
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
    /* Loopback needs no configured interface; anything else does. */
    if (IP4_IS_LOOPBACK(addr)) {
        t->local_ip = addr;
    } else if (!n->ip) {
        return -EADDRNOTAVAIL;
    } else {
        t->local_ip = n->ip;
    }
    if (!t->local_port) {
        t->local_port = tcp_pick_port();
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
    t->snd_max = t->snd_nxt;
    t->last_rcv = timer_jiffies();
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
        !t->fin_sent && !t->fin_pending && t->sndlen < TCP_SNDBUF) {
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
        return t->timed_out ? -ETIMEDOUT : -ECONNRESET;
    }
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) {
        return -ENOTCONN;
    }
    if (t->fin_sent || t->fin_pending) {
        return -EPIPE;
    }

    room = TCP_SNDBUF - t->sndlen;
    if (room == 0) {
        return 0;               /* the caller waits and tries again */
    }
    if (len > room) {
        len = room;
    }
    {
        u32 at = (t->sndstart + t->sndlen) % TCP_SNDBUF;
        u32 first = TCP_SNDBUF - at;

        if (first > len) {
            first = len;
        }
        memcpy(t->sndbuf + at, data, first);
        memcpy(t->sndbuf, (const u8 *)data + first, len - first);
    }
    t->sndlen += len;

    send_data(t);
    return (s32)len;
}

s32 tcp_peek(struct tcpcb *t, void *data, u32 len)
{
    u8 *out = data;
    u32 n = 0, at = t->rcvtail;

    while (n < len && n < rcv_used(t)) {
        out[n++] = t->rcvbuf[at];
        at = (at + 1) % TCP_RCVBUF;
    }
    if (n > 0) {
        return (s32)n;
    }
    if (t->reset) {
        /* A connection that timed out -- retransmission or keepalive --
         * says ETIMEDOUT, as Linux's does; a reset from the peer says
         * ECONNRESET. */
        return t->timed_out ? -ETIMEDOUT : -ECONNRESET;
    }
    if (t->fin_rcvd) {
        return 0;
    }
    return t->state == TCP_CLOSED ? -ENOTCONN : -EAGAIN;
}

int tcp_port_in_use(u16 port)
{
    int i;

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].used && conns[i].local_port == port) {
            return 1;
        }
    }
    return 0;
}

void tcp_release(struct tcpcb *t)
{
    int i;

    if (t->state == TCP_LISTEN) {
        /* Connections that finished their handshake but were never
         * accepted have no owner now: close them too. */
        for (i = 0; i < TCP_MAX_CONNS; i++) {
            struct tcpcb *c = &conns[i];

            if (c->used && c->listener == t) {
                c->listener = 0;
                c->pending = 0;
                tcp_release(c);
            }
        }
    }
    tcp_close(t);               /* frees it outright in some states */
    if (t->used) {
        t->orphan = 1;
        t->orphan_since = timer_jiffies();
    }
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
        /* A connection that timed out -- retransmission or keepalive --
         * says ETIMEDOUT, as Linux's does; a reset from the peer says
         * ECONNRESET. */
        return t->timed_out ? -ETIMEDOUT : -ECONNRESET;
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

    t->fin_pending = 1;
    send_data(t);               /* the rest of the data, then the FIN */
    return 0;
}
