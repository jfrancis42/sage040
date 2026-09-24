/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * loadavg.c - the load average, the same arithmetic Linux uses.
 *
 * WHAT IT MEASURES. Every few seconds the length of the run queue is
 * sampled -- how many tasks are RUNNING or want to run -- and folded
 * into three exponentially-weighted moving averages, decaying over one,
 * five and fifteen minutes. A steady load of N means that, on average,
 * N tasks were ready to run and competing for the one processor. Below
 * 1.00 the machine had time to spare; above it, tasks were waiting.
 *
 * WHAT IT COUNTS, AND WHAT IT DELIBERATELY DOES NOT. The runnable
 * count is RUNNING + READY, with the idle task excluded -- the idle
 * task is always runnable and represents the absence of load, so
 * counting it would peg the average near 1.00 on a machine doing
 * nothing. This is the classic (BSD) load average: the run queue.
 *
 * Linux also counts tasks in uninterruptible sleep -- almost always
 * waiting on disk -- so that a machine thrashing its disk shows load
 * even with nothing on the CPU. This kernel does not, because it does
 * not mark that state: a task waiting for a sector sleeps on a wait
 * queue like any other blocked task (interruptibly), and there is no
 * flag that separates "blocked on disk" from "blocked reading the
 * terminal". Counting all blocked tasks would report every idle shell
 * as load, which is worse than the omission. So the number here is
 * runnable-only, and a disk-bound machine with an idle CPU reads low.
 * Saying so beats a footnote nobody reads.
 *
 * THE FIXED POINT. There is no floating point in the kernel, so the
 * averages are integers with FSHIFT fractional bits: FIXED_1 is 1.00.
 * The decay constants are Linux's, precomputed for a five-second
 * sample: EXP_1 = 1/e^(5/60), and so on. calc_load is Linux's
 * CALC_LOAD macro, unchanged, because the constants only mean anything
 * with exactly this recurrence.
 */
#include "loadavg.h"
#include "timer.h"       /* HZ, via uapi.h */
#include "uapi.h"        /* SI_LOAD_SHIFT */

/* How many active (runnable, non-idle) tasks there are right now. */
extern int task_nr_active(void);

#define FSHIFT   11              /* fractional bits of precision      */
#define FIXED_1  (1u << FSHIFT)  /* 1.00 in this fixed point          */

/*
 * Sample every five seconds. The +1 is Linux's, and it is not
 * cosmetic: a period of exactly 5*HZ ticks tends to beat against other
 * things that also happen on a round five-second boundary and bias the
 * sample. One tick off breaks the resonance.
 */
#define LOAD_FREQ  (5 * HZ + 1)

/* 1/e^(5sec/1min), 1/e^(5sec/5min), 1/e^(5sec/15min), in FSHIFT fixed. */
#define EXP_1   1884
#define EXP_5   2014
#define EXP_15  2037

/* avenrun[i] is FIXED_1-scaled; count is the ticks until the next
 * sample. Starting at LOAD_FREQ means the first sample is one whole
 * period after boot, so a machine is not judged by the flurry of its
 * own start-up. */
static u32 avenrun[3];
static int count = LOAD_FREQ;

/*
 * One step of the recurrence: load decays toward `active`.
 *   load = load*exp + active*(1 - exp)
 * with everything in FSHIFT fixed point. `active` arrives already
 * shifted (n << FSHIFT), so the product terms share the scale and the
 * final >> FSHIFT brings it back. This is CALC_LOAD from the Linux
 * kernel verbatim; the magic numbers above are only correct for it.
 */
static u32 calc_load(u32 load, u32 exp, u32 active)
{
    u32 newload = load * exp + active * (FIXED_1 - exp);

    return newload >> FSHIFT;
}

void loadavg_tick(void)
{
    u32 active;

    if (--count > 0) {
        return;                 /* not a sample tick: nearly all of them */
    }
    count = LOAD_FREQ;

    active = (u32)task_nr_active() << FSHIFT;
    avenrun[0] = calc_load(avenrun[0], EXP_1, active);
    avenrun[1] = calc_load(avenrun[1], EXP_5, active);
    avenrun[2] = calc_load(avenrun[2], EXP_15, active);
}

void loadavg_get(u32 out[3])
{
    int i;

    /* FSHIFT bits internally, SI_LOAD_SHIFT bits on the way out -- the
     * scale struct sysinfo and Linux's sysinfo(2) both use. */
    for (i = 0; i < 3; i++) {
        out[i] = avenrun[i] << (SI_LOAD_SHIFT - FSHIFT);
    }
}
