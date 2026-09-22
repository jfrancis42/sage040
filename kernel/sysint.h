/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sysint.h - what the system call layer's files share with each other.
 *
 * syscall.c is the gate and the calls this system has always had;
 * syslinux.c is the rest of Linux's interface, there for a C library.
 * Both copy arguments across the boundary the same way.
 */
#ifndef SYSINT_H
#define SYSINT_H

#include "kernel.h"

struct pt_regs;

int from_program(void);
int fetch(void *dst, u32 p, u32 len);
int store(u32 p, const void *src, u32 len);
int fetch_str(char *dst, u32 p, u32 max);

s32 syscall_linux(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6,
                  struct pt_regs *regs);

#endif /* SYSINT_H */
