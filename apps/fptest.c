/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fptest - does a task keep its floating point registers?
 *
 * Loads eight distinctive values into fp0-fp7 and a rounding mode into
 * fpcr, then for three seconds gives the processor away -- by yielding
 * and by being preempted -- and checks each time that every one of them
 * is still what it put there. Run two at once, with
 * different values, and a kernel that does not save the FPU on a switch
 * hands each one the other's registers.
 *
 *   fptest N      use values derived from N, report once at the end
 */
#include "ulib.h"

/* For a fixed TIME, not a fixed count: two of these must overlap, and
 * a count that is quick on one host finishes before the second starts. */
#define SECONDS 3

static double want[8];
static double got[8];

static void load(void)
{
    __asm__ volatile (
        "fmove.d (%0),%%fp0\n\t"
        "fmove.d 8(%0),%%fp1\n\t"
        "fmove.d 16(%0),%%fp2\n\t"
        "fmove.d 24(%0),%%fp3\n\t"
        "fmove.d 32(%0),%%fp4\n\t"
        "fmove.d 40(%0),%%fp5\n\t"
        "fmove.d 48(%0),%%fp6\n\t"
        "fmove.d 56(%0),%%fp7\n\t"
        : : "a"(want)
        : "fp0", "fp1", "fp2", "fp3", "fp4", "fp5", "fp6", "fp7", "memory");
}

static void store(void)
{
    __asm__ volatile (
        "fmove.d %%fp0,(%0)\n\t"
        "fmove.d %%fp1,8(%0)\n\t"
        "fmove.d %%fp2,16(%0)\n\t"
        "fmove.d %%fp3,24(%0)\n\t"
        "fmove.d %%fp4,32(%0)\n\t"
        "fmove.d %%fp5,40(%0)\n\t"
        "fmove.d %%fp6,48(%0)\n\t"
        "fmove.d %%fp7,56(%0)\n\t"
        : : "a"(got) : "memory");
}

static u32 get_fpcr(void)
{
    u32 v;

    __asm__ volatile ("fmove.l %%fpcr,%0" : "=d"(v));
    return v;
}

static void set_fpcr(u32 v)
{
    __asm__ volatile ("fmove.l %0,%%fpcr" : : "d"(v));
}

int main(int argc, char **argv)
{
    u32 n = 1, fpcr, rounds = 0, bad = 0, i, until;
    const char *p;

    if (argc > 1) {
        n = 0;
        for (p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            n = n * 10 + (u32)(*p - '0');
        }
    }
    for (i = 0; i < 8; i++) {
        want[i] = (double)(n * 1000 + i) + 0.25;
    }
    /* Round toward zero for one task, to minus infinity for the other:
     * the mode is per task too. */
    fpcr = (n & 1) ? 0x10 : 0x20;
    set_fpcr(fpcr);

    until = times() + SECONDS * HZ;
    while ((s32)(times() - until) < 0) {
        rounds++;
        volatile u32 spin;

        /* Loaded afresh each round: nothing between the store and the
         * next load may be trusted not to use an FP register. */
        load();
        syscall(__NR_sched_yield);
        for (spin = 0; spin < 200; spin++) {
            ;               /* long enough to be preempted sometimes */
        }
        store();
        /* Compared as bits, with integer instructions, so the check
         * itself touches no FP register. */
        for (i = 0; i < 16; i++) {
            if (((u32 *)got)[i] != ((u32 *)want)[i]) {
                bad++;
            }
        }
        if (get_fpcr() != fpcr) {
            bad++;
        }
        if (bad) {
            break;
        }
    }

    puts("fptest ");
    putdec(n);
    puts(bad ? ": FPU STATE LOST after " : ": fpu state kept over ");
    putdec(rounds);
    puts(" yields\n");
    return bad ? 1 : 0;
}
