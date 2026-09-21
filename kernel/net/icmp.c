/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * icmp.c - echo request and echo reply. Ping.
 *
 * Reference: RFC 792.
 *
 * Only the echo pair is implemented, in both directions, and that is
 * the whole of what a machine needs to be a good citizen at this level.
 * Answering an echo is what makes it visible to anyone else; sending one
 * is what makes it able to say whether the network works, which is the
 * first question asked of every stack ever written.
 *
 * Destination-unreachable and time-exceeded are NOT generated. They are
 * a router's job and this is a host; a host that sent them would be
 * announcing which of its ports are closed to anybody who asked.
 */
#include "net.h"
#include "timer.h"
#include "console.h"
#include "errno.h"
#include "string.h"

#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8

struct icmphdr {
    u8  type;
    u8  code;
    u16 check;
    u16 id;
    u16 seq;
} PACKED;

#define ICMP_HDR_LEN    ((int)sizeof(struct icmphdr))
#define PING_MAX_DATA   64

/* What a waiting `ping` is listening for. */
static volatile int reply_seen;
static ip4_t   ping_peer;
static u16     ping_id;
static u16     ping_seq;
static u32     ping_sent_at;
static u32     ping_rtt_ms;

void icmp_input(ip4_t from, const void *data, u32 len)
{
    const struct icmphdr *h = data;

    if (len < (u32)ICMP_HDR_LEN) {
        return;
    }
    if (net_checksum(data, len, 0) != 0) {
        return;
    }

    switch (h->type) {
    case ICMP_ECHO_REQUEST: {
        /*
         * Reply with the payload we were sent, unchanged. Some pingers
         * put a timestamp in it and check it comes back; all of them
         * expect the same bytes, so the packet is copied and the type
         * and checksum rewritten rather than rebuilt.
         */
        u8 buf[ICMP_HDR_LEN + PING_MAX_DATA];
        struct icmphdr *r = (struct icmphdr *)buf;
        u32 n = len;

        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        memcpy(buf, data, n);
        r->type = ICMP_ECHO_REPLY;
        r->check = 0;
        r->check = net_checksum(buf, n, 0);
        ip_output(from, IPPROTO_ICMP, buf, n);
        break;
    }

    case ICMP_ECHO_REPLY:
        if (from == ping_peer && h->id == ping_id && h->seq == ping_seq) {
            ping_rtt_ms = (timer_jiffies() - ping_sent_at) * (1000 / HZ);
            reply_seen = 1;
        }
        break;

    default:
        break;
    }
}

/*
 * One echo request, and wait for its reply.
 *
 * Returns the round trip in milliseconds, or -errno. The id is fixed
 * and the sequence counts up, which is enough to tell this ping's reply
 * from a stale one arriving late.
 */
int icmp_ping(ip4_t to, u32 timeout_ms, u32 *rtt_ms)
{
    u8 buf[ICMP_HDR_LEN + 32];
    struct icmphdr *h = (struct icmphdr *)buf;
    int err;
    u32 i;

    if (!to) {
        return -EINVAL;
    }

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < 32; i++) {
        buf[ICMP_HDR_LEN + i] = (u8)('a' + (i % 26));
    }

    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->id = 0x5a10;             /* "sage", near enough */
    h->seq = ++ping_seq;
    h->check = 0;
    h->check = net_checksum(buf, sizeof(buf), 0);

    ping_peer = to;
    ping_id = h->id;
    reply_seen = 0;
    ping_sent_at = timer_jiffies();

    err = ip_output(to, IPPROTO_ICMP, buf, sizeof(buf));
    if (err < 0) {
        return err;
    }

    if (!net_wait(&reply_seen, timeout_ms)) {
        return -ETIMEDOUT;
    }
    if (rtt_ms) {
        *rtt_ms = ping_rtt_ms;
    }
    return 0;
}
