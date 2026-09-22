/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * types.h - the fixed-width integers everything here is written in.
 *
 * Its own header, at the top of the tree, because three separate things
 * need it and none of them should have to drag in the other two:
 *
 *   tests/sage040.h   the machine's registers
 *   kernel/uapi.h     the system call ABI
 *   kernel/kernel.h   the kernel's own internals
 *
 * Before this existed, uapi.h reached the types by including kernel.h,
 * which includes sage040.h -- so a program asking for `struct stat` got
 * the SM501's register map with it, and could poke a chip having
 * included nothing that said so. The types are the only thing those
 * three genuinely share.
 *
 * No stdint.h: this is a freestanding build, and these names are what
 * the whole tree already reads in.
 */
#ifndef SAGE_TYPES_H
#define SAGE_TYPES_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed char        s8;
typedef signed short       s16;
typedef signed int         s32;
typedef unsigned long long u64;     /* only where Linux's ABI has one */
typedef signed long long   s64;

/* Seconds since 1970-01-01 UTC, as everything Unix counts time. */
typedef u32 time_t;

#endif /* SAGE_TYPES_H */
