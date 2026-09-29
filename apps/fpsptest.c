/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fpsptest - the floating-point functions, checked.
 *
 *     fpsptest        every <math.h> function against the host's libm
 *     fpsptest -i     an F-line instruction that is not floating point:
 *                     it must come back as SIGILL
 *
 * Driven by kernel/fpsptest.sh, which runs it twice: once with the CPU
 * computing these itself (QEMU's softfloat), and once as
 * `-cpu m68040,fpsp-trap=on`, when every one of them traps and
 * Motorola's FPSP in the kernel computes it -- as on a real 68040.
 * Either way the answers are checked against a third implementation,
 * the host's C library, to within 2 units in the last place; and the
 * kernel's count of emulated instructions (KSTAT_FPSP) is printed, so
 * the suite can tell which of the two actually did the work.
 */
#include "ulib.h"
#include <math.h>

struct ref {
    const char *name;
    const char *args;
    double x, y;
    u64 want;
};

/* Generated on the host from Python 3.14.7's math module (the C library's */
/* libm): name, printable arguments, x, y, and the expected bits.       */
static const struct ref refs[] = {
    { "sin", "0.5", 0.5, 0.0, 0x3fdeaee8744b05f0ULL },
    { "sin", "2.0", 2.0, 0.0, 0x3fed18f6ead1b446ULL },
    { "sin", "-3.0", -3.0, 0.0, 0xbfc210386db6d55bULL },
    { "sin", "100.0", 100.0, 0.0, 0xbfe03425b78c4db8ULL },
    { "cos", "0.5", 0.5, 0.0, 0x3fec1528065b7d50ULL },
    { "cos", "2.0", 2.0, 0.0, 0xbfdaa22657537205ULL },
    { "cos", "-3.0", -3.0, 0.0, 0xbfefae04be85e5d2ULL },
    { "cos", "100.0", 100.0, 0.0, 0x3feb981dbf665fdfULL },
    { "tan", "0.5", 0.5, 0.0, 0x3fe17b4f5bf3474aULL },
    { "tan", "1.2", 1.2, 0.0, 0x400493c43acb164dULL },
    { "tan", "-0.3", -0.3, 0.0, 0xbfd3cc2a44e29998ULL },
    { "asin", "0.5", 0.5, 0.0, 0x3fe0c152382d7366ULL },
    { "asin", "-0.9", -0.9, 0.0, 0xbff1ea93705fa172ULL },
    { "acos", "0.5", 0.5, 0.0, 0x3ff0c152382d7366ULL },
    { "acos", "-0.9", -0.9, 0.0, 0x400586476251e745ULL },
    { "atan", "0.5", 0.5, 0.0, 0x3fddac670561bb4fULL },
    { "atan", "10.0", 10.0, 0.0, 0x3ff789bd2c160054ULL },
    { "atan", "-2.0", -2.0, 0.0, 0xbff1b6e192ebbe44ULL },
    { "sinh", "0.5", 0.5, 0.0, 0x3fe0acd00fe63b97ULL },
    { "sinh", "3.0", 3.0, 0.0, 0x40240926e70949aeULL },
    { "cosh", "0.5", 0.5, 0.0, 0x3ff20ac1862ae8d0ULL },
    { "cosh", "3.0", 3.0, 0.0, 0x402422a497d6185eULL },
    { "tanh", "0.5", 0.5, 0.0, 0x3fdd9353d7568af3ULL },
    { "tanh", "3.0", 3.0, 0.0, 0x3fefd77d111a0b00ULL },
    { "atanh", "0.5", 0.5, 0.0, 0x3fe193ea7aad030bULL },
    { "atanh", "-0.9", -0.9, 0.0, 0xbff78e360604b32dULL },
    { "exp", "1.0", 1.0, 0.0, 0x4005bf0a8b145769ULL },
    { "exp", "-2.5", -2.5, 0.0, 0x3fb50385c094f425ULL },
    { "exp", "10.0", 10.0, 0.0, 0x40d5829dcf950560ULL },
    { "expm1", "0.001", 0.001, 0.0, 0x3f506466dfb8cf3aULL },
    { "expm1", "1.0", 1.0, 0.0, 0x3ffb7e151628aed2ULL },
    { "exp2", "0.5", 0.5, 0.0, 0x3ff6a09e667f3bcdULL },
    { "exp2", "10.25", 10.25, 0.0, 0x409306fe0a31b715ULL },
    { "exp2", "-3.0", -3.0, 0.0, 0x3fc0000000000000ULL },
    { "exp10", "0.5", 0.5, 0.0, 0x40094c583ada5b53ULL },
    { "exp10", "-2.0", -2.0, 0.0, 0x3f847ae147ae147bULL },
    { "exp10", "3.0", 3.0, 0.0, 0x408f400000000000ULL },
    { "log", "2.0", 2.0, 0.0, 0x3fe62e42fefa39efULL },
    { "log", "0.1", 0.1, 0.0, 0xc0026bb1bbb55515ULL },
    { "log", "1000.0", 1000.0, 0.0, 0x401ba18a998fffa0ULL },
    { "log1p", "0.001", 0.001, 0.0, 0x3f5060354f8c3ebfULL },
    { "log1p", "1.0", 1.0, 0.0, 0x3fe62e42fefa39efULL },
    { "log2", "3.0", 3.0, 0.0, 0x3ff95c01a39fbd68ULL },
    { "log2", "1024.5", 1024.5, 0.0, 0x4024005c4f58bde5ULL },
    { "log10", "2.0", 2.0, 0.0, 0x3fd34413509f79ffULL },
    { "log10", "100000.0", 100000.0, 0.0, 0x4014000000000000ULL },
    { "trunc", "-2.7", -2.7, 0.0, 0xc000000000000000ULL },
    { "trunc", "2.7", 2.7, 0.0, 0x4000000000000000ULL },
    { "rint", "2.5", 2.5, 0.0, 0x4000000000000000ULL },
    { "rint", "3.5", 3.5, 0.0, 0x4010000000000000ULL },
    { "rint", "-1.5", -1.5, 0.0, 0xc000000000000000ULL },
    { "fmod", "10.0, 3.0", 10.0, 3.0, 0x3ff0000000000000ULL },
    { "fmod", "-7.5, 2.0", -7.5, 2.0, 0xbff8000000000000ULL },
    { "remainder", "10.0, 3.0", 10.0, 3.0, 0x3ff0000000000000ULL },
    { "remainder", "11.0, 3.0", 11.0, 3.0, 0xbff0000000000000ULL },
    { "ldexp", "3.0, 10.0", 3.0, 10.0, 0x40a8000000000000ULL },
    { "ldexp", "1.0, -3.0", 1.0, -3.0, 0x3fc0000000000000ULL },
};
/* sincos(0.7), and pi for fmovecr #0. */
#define SINCOS_X 0.7
#define SINCOS_S 0x3fe49d6e694619b8ULL
#define SINCOS_C 0x3fe87996529f9d93ULL
#define PI_BITS  0x400921fb54442d18ULL

static int failures;

static u64 bits(double d)
{
    union { double d; u64 u; } v;

    v.d = d;
    return v.u;
}

/* Distance in units in the last place: the bit patterns, ordered as
 * integers, so that adjacent doubles are one apart. */
static u64 ulps(u64 a, u64 b)
{
    s64 ia = (a >> 63) ? (s64)(0x8000000000000000ULL - a) : (s64)a;
    s64 ib = (b >> 63) ? (s64)(0x8000000000000000ULL - b) : (s64)b;

    return (u64)(ia > ib ? ia - ib : ib - ia);
}

static void puthex64(u64 v)
{
    puthex((u32)(v >> 32));
    puthex((u32)v);
}

static void check(const char *name, const char *args, double got, u64 want)
{
    u64 g = bits(got), d = ulps(g, want);

    puts(d <= 2 ? "  ok   " : "  FAIL ");
    puts(name);
    putch('(');
    puts(args);
    putch(')');
    if (d > 2) {
        puts(": got 0x");
        puthex64(g);
        puts(", want 0x");
        puthex64(want);
        failures++;
    }
    putch('\n');
}

static double call(const struct ref *r)
{
    const char *n = r->name;

    if (!strcmp(n, "sin"))       return sin(r->x);
    if (!strcmp(n, "cos"))       return cos(r->x);
    if (!strcmp(n, "tan"))       return tan(r->x);
    if (!strcmp(n, "asin"))      return asin(r->x);
    if (!strcmp(n, "acos"))      return acos(r->x);
    if (!strcmp(n, "atan"))      return atan(r->x);
    if (!strcmp(n, "sinh"))      return sinh(r->x);
    if (!strcmp(n, "cosh"))      return cosh(r->x);
    if (!strcmp(n, "tanh"))      return tanh(r->x);
    if (!strcmp(n, "atanh"))     return atanh(r->x);
    if (!strcmp(n, "exp"))       return exp(r->x);
    if (!strcmp(n, "expm1"))     return expm1(r->x);
    if (!strcmp(n, "exp2"))      return exp2(r->x);
    if (!strcmp(n, "exp10"))     return exp10(r->x);
    if (!strcmp(n, "log"))       return log(r->x);
    if (!strcmp(n, "log1p"))     return log1p(r->x);
    if (!strcmp(n, "log2"))      return log2(r->x);
    if (!strcmp(n, "log10"))     return log10(r->x);
    if (!strcmp(n, "trunc"))     return trunc(r->x);
    if (!strcmp(n, "rint"))      return rint(r->x);
    if (!strcmp(n, "fmod"))      return fmod(r->x, r->y);
    if (!strcmp(n, "remainder")) return remainder(r->x, r->y);
    if (!strcmp(n, "ldexp"))     return ldexp(r->x, (int)r->y);
    puts("fpsptest: no function called ");
    puts(n);
    putch('\n');
    failures++;
    return 0;
}

static void fpsp_counts(struct fpspstats *s)
{
    memset(s, 0, sizeof(*s));
    syscall(__NR_kstat, KSTAT_FPSP, sizeof(*s), (u32)s);
}

static void on_sigill(int sig)
{
    (void)sig;
    puts("fpsptest: SIGILL, as it should be\n");
    exit(0);
}

int main(int argc, char **argv)
{
    struct fpspstats before, after;
    u32 i;
    double s, c, pi;

    if (argc > 1 && !strcmp(argv[1], "-i")) {
        /*
         * cpid 7: an F-line pattern that is no coprocessor instruction
         * at all. The FPSP reads it out of this program's memory, finds
         * it is not floating point, and reports it (real_fline).
         */
        signal(SIGILL, on_sigill);
        __asm__ volatile(".word 0xfe00");
        puts("fpsptest: the F-line instruction did NOT trap\n");
        return 1;
    }

    fpsp_counts(&before);

    for (i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        check(refs[i].name, refs[i].args, call(&refs[i]), refs[i].want);
    }

    sincos(SINCOS_X, &s, &c);
    check("sincos.sin", "0.7", s, SINCOS_S);
    check("sincos.cos", "0.7", c, SINCOS_C);

    /* fmovecr #0: pi, from the FPU's own constant ROM. */
    __asm__("fmovecr.x #0,%0" : "=f"(pi));
    check("fmovecr", "#0", pi, PI_BITS);

    fpsp_counts(&after);
    puts("fpsptest: ");
    putdec(after.unimp - before.unimp);
    puts(" instructions completed by the FPSP, ");
    putdec(after.reported - before.reported);
    puts(" exceptions reported\n");

    puts(failures ? "fpsptest: FAILED\n" : "fpsptest: all right\n");
    return failures != 0;
}
