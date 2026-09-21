/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * random.h - numbers that are hard to guess, within reason.
 *
 * NOT CRYPTOGRAPHIC. See random.c for exactly what that means and what
 * this is for. The short version: it is good enough that somebody at
 * the far end of a wire cannot predict it, and nowhere near good enough
 * to keep a secret from somebody with the machine.
 */
#ifndef RANDOM_H
#define RANDOM_H

#include "kernel.h"

/* Seed from the clock, the tick and the ethernet address. Call once the
 * drivers are up, because that is when those exist. */
void random_init(void);

/* Stir something in. Anything unpredictable will do. */
void random_add(u32 v);

u32  random_u32(void);

#endif /* RANDOM_H */
