/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * spin - a program that will not stop on its own.
 *
 * `cube` looks like an interruptible program and is not: it polls the
 * keyboard and exits on any key, so ctrl-C appears to work on it whether
 * or not signals do anything at all. This one has no such politeness.
 * It never reads, never exits, and ignores everything, so the only way
 * out of it is the kernel taking it away -- which is exactly the claim
 * worth testing.
 *
 *   spin        a bare loop: no system calls whatsoever, so only the
 *               timer interrupt can ever notice a ctrl-C
 *   spin calls  a loop that makes a system call each time round, so the
 *               boundary path is what notices
 *
 * The second is the one ctrl-Z needs: a program can only be STOPPED at a
 * system call boundary, because that is the only place there is a
 * context worth coming back to.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int syscalls = argc > 1 && strcmp(argv[1], "calls") == 0;
    volatile u32 n = 0;

    puts(syscalls ? "SPINNING-WITH-CALLS\n" : "SPINNING-SILENTLY\n");

    for (;;) {
        n++;
        if (syscalls) {
            times();
        }
    }
}
