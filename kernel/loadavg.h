/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * loadavg.h - the load average.
 *
 * The one number a machine keeps ABOUT ITSELF OVER TIME rather than at
 * an instant: how many tasks wanted the processor, averaged over the
 * last one, five and fifteen minutes. `uptime` and `w` print it.
 */
#ifndef LOADAVG_H
#define LOADAVG_H

#include "kernel.h"

/* Called from every timer tick; samples the run queue every few seconds
 * and folds it into the three averages. Cheap on the ticks between. */
void loadavg_tick(void);

/*
 * The three averages, most recent first (1, 5, 15 minutes), as fixed
 * point with SI_LOAD_SHIFT fractional bits -- the form struct sysinfo
 * carries and the form Linux's sysinfo(2) returns, so a program divides
 * by (1 << SI_LOAD_SHIFT) to get the real number.
 */
void loadavg_get(u32 out[3]);

#endif /* LOADAVG_H */
