/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * hello.c - the smallest possible program.
 *
 * It exists to prove that running programs is a general facility and not
 * one thing that happens to work: it shares no code with the cube, draws
 * nothing, touches no hardware, and reaches the system only through
 * write(), uname() and exit().
 *
 * It also shows argv arriving intact, and a non-zero exit status coming
 * back to the shell -- `hello -x` exits 1, and the shell says so.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    struct utsname u;
    int i;

    puts("hello from a program\n");

    if (uname(&u) == 0) {
        puts("  running on ");
        puts(u.sysname);
        putch(' ');
        puts(u.release);
        puts(" (");
        puts(u.machine);
        puts(")\n");
    }

    puts("  argc = ");
    putdec((u32)argc);
    putch('\n');
    for (i = 0; i < argc; i++) {
        puts("  argv[");
        putdec((u32)i);
        puts("] = ");
        puts(argv[i]);
        putch('\n');
    }

    /* A visible non-zero exit, so the shell's reporting of one can be
     * seen working rather than assumed. */
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'x') {
        puts("  exiting 1 on purpose\n");
        return 1;
    }
    return 0;
}
