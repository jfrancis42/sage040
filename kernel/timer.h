/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * timer.h - the kernel's notion of time passing.
 *
 * A counter incremented by whichever timer driver registered itself, and
 * the things that can be built from it. No hardware here: the driver
 * knows about prescalers and channels, and this does not.
 *
 * HZ is 100, which is what Linux used for most of its life and what a
 * machine of this vintage would have chosen. It makes a tick 10 ms --
 * fine enough that a 50 fps frame is exactly two of them, coarse enough
 * that the handler is nowhere near the foreground's throat. A tick
 * faster than its own handler starves everything else, which is a
 * mistake this machine has already made once with an MFP timer at 13 us.
 */
#ifndef TIMER_H
#define TIMER_H

#include "kernel.h"
#include "uapi.h"

/*
 * HZ lives in uapi.h, not here. times() returns ticks and a tick count
 * is meaningless without the rate, so the rate crosses the system call
 * boundary with it -- which makes it part of the ABI rather than one of
 * the kernel's own numbers.
 */

/*
 * Ticks since boot. Written by the interrupt handler and read
 * everywhere, so volatile -- without it the compiler is entitled to hoist
 * the read out of a wait loop and spin forever on a value it decided
 * could not change.
 */
extern volatile u32 jiffies;

/* Called by the timer driver's interrupt handler, and by nothing else. */
void timer_tick(void);

u32  timer_jiffies(void);

/*
 * The time of day, to the tick: the one clock that time(),
 * gettimeofday() and file timestamps all read. clock_set() sets the RTC
 * as well. See timer.c.
 */
struct timeval;
void clock_get(struct timeval *tv);
int  clock_set(const struct timeval *tv);
u32  timer_uptime_ms(void);

/*
 * Wait. Returns 0, or -ENODEV if no timer is running -- in which case it
 * does not wait at all, because a sleep with nothing to wake it is a
 * hang, and returning an error is the honest version of that.
 */
int  timer_sleep_ticks(u32 ticks);
int  timer_sleep_ms(u32 ms);

/* Bring the registered timer up at HZ. */
int  timer_start(void);

#endif /* TIMER_H */
