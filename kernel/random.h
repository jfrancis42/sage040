/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * random.h - the kernel's random number generator: an entropy pool fed
 * by interrupt timing, and ChaCha20 output. See random.c.
 */
#ifndef RANDOM_H
#define RANDOM_H

#include "kernel.h"

/* Seed from the clock, the tick and the ethernet address. Call once the
 * drivers are up, because that is when those exist. */
void random_init(void);

/*
 * An interrupt arrived: `where` is the timer's position within its tick
 * at that moment, and `what` which interrupt it was. Called from the
 * interrupt dispatcher, so it only records; the pool is stirred later.
 * `bits_x8` is the entropy it is credited with, in eighths of a bit.
 */
void random_interrupt(u32 where, u32 what, u32 bits_x8);

/* Mix bytes in without crediting them -- what a write to /dev/random or
 * /dev/urandom does, as on Linux. */
void random_write(const void *buf, u32 len);

/* Has the pool been credited with enough to be unpredictable? */
int  random_ready(void);

/*
 * Output. random_get never waits: before the pool is ready it gives the
 * best it has, as Linux's get_random_bytes and /dev/urandom do.
 * random_wait sleeps until the pool is ready (0), or returns -EAGAIN
 * with `nonblock`, or -EINTR on a signal: getrandom() and /dev/random.
 */
void random_get(void *buf, u32 len);
int  random_wait(int nonblock);

u32  random_u32(void);

#endif /* RANDOM_H */
