/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * random.c - numbers that are hard to guess, within reason.
 *
 * BE CLEAR ABOUT WHAT THIS IS NOT. It is not a cryptographic generator
 * and must not be used as one. There is no entropy pool, no reseeding
 * from interrupt timing, and an attacker who learns the state can
 * predict every future value. What it is for is the handful of places
 * where a number has to be hard for a REMOTE party to guess without
 * being secret from anyone with the machine in front of them -- above
 * all a TCP initial sequence number.
 *
 * Why that matters enough to have a file. An ISN taken from the clock is
 * guessable by anybody who knows roughly when a connection was made, and
 * an off-path attacker who can guess it can inject data into somebody
 * else's connection without ever seeing it. It was `jiffies * 7919`
 * here, which is to say it was a multiplication of a number that
 * increases by one every ten milliseconds.
 *
 * The generator is xorshift32 -- three shifts and three xors, a full
 * period of 2^32-1, and good enough statistical behaviour that
 * successive values do not visibly correlate. It is seeded from the
 * things this machine has that differ between boots and between
 * machines: the real-time clock, the tick, and the ethernet address.
 */
#include "random.h"
#include "timer.h"
#include "dev.h"
#include "string.h"

static u32 state = 0x2545f491;  /* never zero: xorshift is stuck at zero */

/*
 * Mix something into the state.
 *
 * A multiply by an odd constant and an xor-shift-fold, which is enough
 * to spread the bits of a poor input across the whole word. Feeding
 * seeds in directly would leave a seed with few set bits producing a
 * state with few set bits.
 */
static void mix(u32 v)
{
    v *= 0x9e3779b1UL;          /* the golden ratio, as everybody uses */
    v ^= v >> 15;
    state ^= v;
    state += 0x6d2b79f5UL;
    if (!state) {
        state = 0x2545f491;
    }
}

void random_init(void)
{
    struct rtcdev *r = dev_rtc();
    struct netdev *n = dev_first_net();
    time_t now = 0;

    /*
     * The clock differs between boots, the MAC between machines, and
     * the tick counter captures how long startup happened to take --
     * which varies with how the disk responded. None of it is secret;
     * all of it is unknown to somebody on the far end of a wire.
     */
    if (r && r->get(r, &now) == 0) {
        mix((u32)now);
    }
    mix(timer_jiffies());
    if (n) {
        mix(((u32)n->mac[2] << 24) | ((u32)n->mac[3] << 16) |
            ((u32)n->mac[4] << 8) | (u32)n->mac[5]);
    }
    mix((u32)(unsigned long)&state);    /* where the kernel landed */
}

void random_add(u32 v)
{
    mix(v);
}

u32 random_u32(void)
{
    /* xorshift32, Marsaglia's 13/17/5 triple. */
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
