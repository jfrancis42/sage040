/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * cdtest - change directory, report, and leave.
 *
 * The working directory used to be a single static inside the
 * filesystem, so this program running `chdir("/bin")` moved the SHELL
 * too, and every other task with it. A program has to be able to walk
 * wherever it likes without the thing that started it ending up
 * somewhere else.
 *
 * Run it as `cdtest /bin` and then `pwd`: the shell must still be where
 * it was.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[64];

    if (argc < 2) {
        puts("usage: cdtest DIR\n");
        return 1;
    }

    if (getcwd(buf, sizeof(buf)) >= 0) {
        puts("cdtest: started in ");
        puts(buf);
        putch('\n');
    }

    if (chdir(argv[1]) < 0) {
        puts("cdtest: cannot chdir to ");
        puts(argv[1]);
        putch('\n');
        return 1;
    }

    if (getcwd(buf, sizeof(buf)) >= 0) {
        puts("cdtest: now in ");
        puts(buf);
        putch('\n');
    }
    return 0;
}
