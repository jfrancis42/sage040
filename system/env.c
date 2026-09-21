/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * env - print the environment this program was started with.
 *
 * Small, and the point of it is to prove something that is otherwise
 * invisible: that the shell's environment actually crossed into a
 * different address space. The shell's own `env` builtin prints the
 * shell's copy and would look identical whether or not any of it
 * reached a program.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int i;

    if (argc > 1) {
        /* `env NAME` prints one, which is what a script wants. */
        for (i = 1; i < argc; i++) {
            const char *v = getenv(argv[i]);

            if (v) {
                puts(v);
                puts("\n");
            } else {
                return 1;
            }
        }
        return 0;
    }

    if (!environ) {
        eputs("env: nothing was passed\n");
        return 1;
    }
    for (i = 0; environ[i]; i++) {
        puts(environ[i]);
        puts("\n");
    }
    return 0;
}
