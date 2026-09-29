/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * math.h - the floating-point unit's own functions, for ulib programs.
 *
 * Each of these is one MC68881/MC68882 instruction. The MC68040 has only
 * some of them in silicon: fsqrt, fabs, fneg, and the four arithmetic
 * operations are hardware; fsin, fcos, fetox, flogn and the rest of the
 * transcendentals are not (MC68040 User's Manual, Table 9-10). On the
 * real chip those raise the unimplemented floating-point instruction
 * exception, and the kernel's copy of Motorola's M68040 Floating-Point
 * Software Package computes the result to the 68881's precision and
 * resumes the program -- which is exactly how Motorola meant them to
 * be used, and why nothing here reimplements them. Under QEMU they run
 * directly unless the machine is started with fpsp-trap=on, which makes
 * the emulated chip trap as the real one does.
 *
 * A program built with picolibc has picolibc's libm instead, which is C
 * and needs none of this.
 *
 * Double precision in and out; the unit works in extended internally.
 */
#ifndef ULIB_MATH_H
#define ULIB_MATH_H

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_SQRT2     1.41421356237309504880

#define ULIB_FP1(name, insn)                                        \
    static inline double name(double x)                             \
    {                                                               \
        double r;                                                   \
        __asm__(insn ".x %1,%0" : "=f"(r) : "f"(x));                \
        return r;                                                   \
    }

/* In silicon on the 68040. */
ULIB_FP1(sqrt,  "fsqrt")
ULIB_FP1(fabs,  "fabs")

/* Completed by the FPSP on the 68040. */
ULIB_FP1(sin,   "fsin")
ULIB_FP1(cos,   "fcos")
ULIB_FP1(tan,   "ftan")
ULIB_FP1(asin,  "fasin")
ULIB_FP1(acos,  "facos")
ULIB_FP1(atan,  "fatan")
ULIB_FP1(sinh,  "fsinh")
ULIB_FP1(cosh,  "fcosh")
ULIB_FP1(tanh,  "ftanh")
ULIB_FP1(atanh, "fatanh")
ULIB_FP1(exp,   "fetox")
ULIB_FP1(expm1, "fetoxm1")
ULIB_FP1(exp2,  "ftwotox")
ULIB_FP1(exp10, "ftentox")
ULIB_FP1(log,   "flogn")
ULIB_FP1(log1p, "flognp1")
ULIB_FP1(log2,  "flog2")
ULIB_FP1(log10, "flog10")
ULIB_FP1(trunc, "fintrz")
ULIB_FP1(rint,  "fint")         /* in the current rounding mode       */

#undef ULIB_FP1

/* fsincos: both at once, one trap on the 68040 rather than two. */
static inline void sincos(double x, double *s, double *c)
{
    double rs, rc;

    __asm__("fsincos.x %2,%1:%0" : "=f"(rs), "=f"(rc) : "f"(x));
    *s = rs;
    *c = rc;
}

/* fmod and remainder are the FPU's fmod and frem: dyadic, so the
 * dividend is the destination register. */
static inline double fmod(double x, double y)
{
    __asm__("fmod.x %1,%0" : "+f"(x) : "f"(y));
    return x;
}

static inline double remainder(double x, double y)
{
    __asm__("frem.x %1,%0" : "+f"(x) : "f"(y));
    return x;
}

/* x * 2^n, by fscale. */
static inline double ldexp(double x, int n)
{
    __asm__("fscale.l %1,%0" : "+f"(x) : "d"(n));
    return x;
}

#endif /* ULIB_MATH_H */
