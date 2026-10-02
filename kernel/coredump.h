/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
#ifndef COREDUMP_H
#define COREDUMP_H

#include "kernel.h"

struct task;

/*
 * Write `core` for a program `sig` is ending, if that signal dumps core
 * and the program's RLIMIT_CORE allows it (coredump.c). `dregs` is
 * d0-d7 then a0-a6, as the exception and system call entries save
 * them; sr, pc and the format word are the hardware frame's. Returns 1
 * if a core was written -- the wait status's WCOREDUMP -- else 0.
 */
int core_dump(struct task *t, int sig, const u32 *dregs,
              u16 sr, u32 pc, u16 fmtvec);

#endif /* COREDUMP_H */
