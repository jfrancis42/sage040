/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * statfs - exercise fstat, access, dup and isatty.
 *
 * These are the calls a ported program makes constantly and that this
 * machine did not have. Each one is checked against something the
 * caller can see for itself, so a wrong answer shows up as a wrong
 * line rather than as a program that quietly does the wrong thing.
 */
#include "ulib.h"

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

int main(int argc, char **argv)
{
    struct stat st;
    int fd, fd2;
    const char *path = argc > 1 ? argv[1] : "/etc/rc";

    /* stdin is the terminal; a file is not. */
    report("isatty(0) says the console is a terminal", isatty(0) == 1);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        puts("statfs: cannot open ");
        puts(path);
        putch('\n');
        return 1;
    }

    report("isatty() on a file says no", isatty(fd) == 0);
    report("fstat on an open file works", fstat(fd, &st) == 0);
    report("  and reports a regular file", S_ISREG(st.st_mode));
    {
        struct stat byname;

        /* The same size stat() gives by name, and the same as seeking to
         * the end -- not merely "more than zero", which a read of any
         * nonzero memory passed. */
        report("  with its real size",
               stat(path, &byname) == 0 && st.st_size == byname.st_size &&
               st.st_size > 0 &&
               lseek(fd, 0, SEEK_END) == (s32)st.st_size);
        lseek(fd, 0, SEEK_SET);
    }

    /* dup shares the position: reading through one moves the other. */
    fd2 = dup(fd);
    report("dup returned a new descriptor", fd2 >= 0 && fd2 != fd);
    if (fd2 >= 0) {
        char a[4], b[4];
        s32 na = read(fd, a, sizeof(a));
        s32 nb = read(fd2, b, sizeof(b));

        report("  and shares the file position", na == 4 && nb == 4 &&
               a[0] != b[0]);
        close(fd2);
    }
    close(fd);

    report("access(F_OK) finds a file that is there", access(path, F_OK) == 0);
    report("access(F_OK) refuses one that is not",
           access("/NOPE.TXT", F_OK) < 0);
    report("access(X_OK) says a text file is not executable",
           access(path, X_OK) < 0);
    report("access(X_OK) says a program is",
           access("/HELLO", X_OK) == 0);

    puts("statfs: done\n");
    return 0;
}
