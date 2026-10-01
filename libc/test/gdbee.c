/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * gdbee.c - something for gdb to debug (kernel/gdbtest.sh).
 *
 * Built -g -O0, so every variable is where the debug information says
 * and every line is a statement. What the test asks of gdb is answered
 * by values fixed here: square(7) is 49, the loop's total is 4950,
 * `greeting` is a string, and `depth` three frames down is 3. A
 * SIGUSR1 is raised and handled, pick() returns a pointer (in %d0), so that gdb has a signal to report
 * and pass on, and the program exits 5.
 */
#include <signal.h>
#include <stdio.h>

static const char *greeting = "hello from gdbee";
static volatile int handled;

struct point { int x, y; };

static void on_usr1(int sig)
{
    (void)sig;
    handled = 1;
}

static int square(int n)
{
    int result = n * n;
    return result;
}

/* A pointer comes back in %d0 from this compiler, not in %a0 as on
 * Linux: `finish` out of this is the check that gdb knows. */
static const char *pick(int which)
{
    return which ? greeting : "other";
}

static int nest(int depth)
{
    if (depth == 3)
        return square(depth + 4);       /* square(7): the backtrace test */
    return nest(depth + 1);
}

int main(int argc, char **argv)
{
    struct point p = { 12, -34 };
    int total = 0;
    int i;

    (void)argv;
    for (i = 0; i < 100; i++)
        total += i;                     /* 4950 */
    printf("%s: total %d\n", greeting, total);
    printf("nest %d\n", nest(argc));
    printf("pick %s\n", pick(argc));
    signal(SIGUSR1, on_usr1);
    raise(SIGUSR1);
    printf("handled %d, point %d,%d\n", handled, p.x, p.y);
    return 5;
}
