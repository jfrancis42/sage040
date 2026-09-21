/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sigtest - the system call gate, and signals.
 *
 * First, that a system call hands every register back as it found it
 * except d0, which carries the result. The gate saves all fifteen and
 * a signal handler is started by rewriting them, so this is the ground
 * everything else here stands on.
 */
#include "ulib.h"

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

/*
 * Load a known value into every register the ABI lets a call keep,
 * make a call, and store them all back. d0 is the call number going in
 * and the result coming out; d1 is getpid's (unused) first argument and
 * must survive too.
 */
static u32 after[14];

static s32 regs_across_a_call(void)
{
    register u32 d0 __asm__("d0") = __NR_getpid;
    register u32 *out __asm__("a1") = after;

    __asm__ volatile (
        "movem.l %%d2-%%d7/%%a2-%%a6,-(%%sp)\n\t"
        "move.l  %%a1,-(%%sp)\n\t"
        "move.l  #0x11111111,%%d1\n\t"
        "move.l  #0x22222222,%%d2\n\t"
        "move.l  #0x33333333,%%d3\n\t"
        "move.l  #0x44444444,%%d4\n\t"
        "move.l  #0x55555555,%%d5\n\t"
        "move.l  #0x66666666,%%d6\n\t"
        "move.l  #0x77777777,%%d7\n\t"
        "move.l  #0x10000000,%%a0\n\t"
        "move.l  #0x10000001,%%a1\n\t"
        "move.l  #0x10000002,%%a2\n\t"
        "move.l  #0x10000003,%%a3\n\t"
        "move.l  #0x10000004,%%a4\n\t"
        "move.l  #0x10000005,%%a5\n\t"
        "move.l  #0x10000006,%%a6\n\t"
        "trap    #0\n\t"
        "move.l  %%a1,-(%%sp)\n\t"          /* free a1 to reach `after` */
        "move.l  4(%%sp),%%a1\n\t"
        "movem.l %%d1-%%d7/%%a0,(%%a1)\n\t" /* 8 longs */
        "move.l  (%%sp)+,32(%%a1)\n\t"      /* the saved a1 */
        "movem.l %%a2-%%a6,36(%%a1)\n\t"    /* 5 longs, to 56 */
        "addq.l  #4,%%sp\n\t"
        "movem.l (%%sp)+,%%d2-%%d7/%%a2-%%a6\n\t"
        : "+d"(d0), "+a"(out)
        :
        : "d1", "a0", "memory", "cc");
    return (s32)d0;
}

static void test_gate(void)
{
    static const u32 want[14] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
        0x55555555, 0x66666666, 0x77777777,
        0x10000000, 0x10000001, 0x10000002, 0x10000003,
        0x10000004, 0x10000005, 0x10000006,
    };
    s32 r = regs_across_a_call();
    int i, ok = 1;

    report("getpid through a hand-written trap returns the pid",
           r == syscall(__NR_getpid));
    for (i = 0; i < 14; i++) {
        ok &= after[i] == want[i];
    }
    report("  and d1-d7, a0-a6 all came back unchanged", ok);
}

int main(void)
{
    test_gate();
    puts("sigtest: done\n");
    return 0;
}
