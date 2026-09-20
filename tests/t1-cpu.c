/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * t1-cpu.c - MC68040 core: supervisor state, VBR, integer ops, on-chip FPU.
 *
 * Reference: Motorola M68040 User's Manual.
 */
#include "sage040.h"

static u16 get_sr(void)
{
    u16 sr;
    __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
    return sr;
}

static u32 get_vbr(void)
{
    u32 vbr;
    __asm__ volatile ("movec %%vbr,%0" : "=d"(vbr));
    return vbr;
}

/*
 * 68020+ 32x32->64 multiply: exercises the wider integer unit.
 *
 * mulu.l <ea>,Dh:Dl multiplies Dl by <ea> and leaves the 64-bit product
 * in Dh:Dl.  So the multiplicand must be loaded into the low output
 * register and the multiplier supplied as the source operand.
 */
static void mulu64(u32 a, u32 b, u32 *hi, u32 *lo)
{
    u32 h, l = a;
    __asm__ volatile ("mulu.l %2,%0:%1" : "=d"(h), "+d"(l) : "d"(b));
    *hi = h;
    *lo = l;
}

int main(void)
{
    double d;
    float f;
    u32 hi, lo;
    u16 sr;

    test_begin("t1 MC68040 core");

    /* --- supervisor state --- */
    sr = get_sr();
    uart_puts("  SR          = 0x"); uart_puthex16(sr); uart_putc('\n');
    if (sr & 0x2000) {
        test_ok("running in supervisor mode (SR.S set)");
    } else {
        test_fail("SR.S clear - not in supervisor mode");
    }

    /* --- VBR, set by crt0 --- */
    uart_puts("  VBR         = 0x"); uart_puthex32(get_vbr()); uart_putc('\n');
    if (get_vbr() == 0) {
        test_ok("VBR points at the vector table at 0x0");
    } else {
        test_fail("VBR is not where crt0 put it");
    }

    /* --- 32x32->64 multiply (68020+) --- */
    mulu64(0x12345678u, 0x10u, &hi, &lo);
    uart_puts("  0x12345678 * 0x10 = 0x");
    uart_puthex32(hi); uart_puthex32(lo); uart_putc('\n');
    if (hi == 0x1u && lo == 0x23456780u) {
        test_ok("mulu.l 32x32->64 correct");
    } else {
        test_fail("mulu.l 32x32->64 WRONG");
    }

    /* --- on-chip FPU, double precision --- */
    d = 1.0;
    d = d / 3.0;
    d = d * 3.0;
    if (d > 0.9999999 && d < 1.0000001) {
        test_ok("FPU double divide/multiply round-trips");
    } else {
        test_fail("FPU double arithmetic WRONG");
    }

    /* --- FPU vs a known IEEE-754 bit pattern --- */
    d = 3.14159265358979323846;
    {
        const u32 *p = (const u32 *)&d;
        uart_puts("  pi (double) = 0x");
        uart_puthex32(p[0]); uart_putc(' '); uart_puthex32(p[1]);
        uart_putc('\n');
        /* IEEE-754 binary64 for pi is 0x400921FB54442D18, big-endian. */
        if (p[0] == 0x400921FBu && p[1] == 0x54442D18u) {
            test_ok("pi matches IEEE-754 binary64 exactly");
        } else {
            test_fail("pi bit pattern WRONG");
        }
    }

    /* --- single precision --- */
    f = 1.0f / 3.0f;
    {
        const u32 *p = (const u32 *)&f;
        uart_puts("  1/3 (single)= 0x"); uart_puthex32(p[0]); uart_putc('\n');
        if (p[0] == 0x3EAAAAABu) {
            test_ok("1/3 matches IEEE-754 binary32 exactly");
        } else {
            test_fail("1/3 bit pattern WRONG");
        }
    }

    /* --- square root, an FPU instruction not synthesisable from + - * --- */
    d = 2.0;
    __asm__ volatile ("fsqrt.d %1,%%fp0\n\tfmove.d %%fp0,%0"
                      : "=m"(d) : "m"(d) : "fp0");
    {
        const u32 *p = (const u32 *)&d;
        uart_puts("  sqrt(2)     = 0x");
        uart_puthex32(p[0]); uart_putc(' '); uart_puthex32(p[1]);
        uart_putc('\n');
        if (p[0] == 0x3FF6A09Eu && p[1] == 0x667F3BCDu) {
            test_ok("fsqrt gives the exact IEEE-754 sqrt(2)");
        } else {
            test_fail("fsqrt WRONG");
        }
    }

    test_end();
    return 0;
}
