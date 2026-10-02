/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * crashee - a program that dies on purpose, for kernel/coretest.sh.
 *
 *   crashee segv        write through a null pointer, from boom()
 *   crashee abort       abort()
 *   crashee sleep       sleep, to be killed by a signal from outside
 *   crashee wait MODE   run `crashee MODE` as a child and report how it
 *                       ended: "signal=N core=0|1"
 *
 * Before dying it leaves a known value in a global (marker) and one in
 * a local of boom() (here), which is what the core is checked for: gdb
 * must read them back out of the file. Built -g -O0 so it can.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

volatile unsigned marker;

static void __attribute__((noinline)) boom(unsigned here)
{
    marker = 0x5a5a1234;
    *(volatile unsigned *)0 = here;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "segv") == 0) {
        boom(0xc0ffee);
    } else if (argc > 1 && strcmp(argv[1], "abort") == 0) {
        marker = 0x5a5a1234;
        abort();
    } else if (argc > 1 && strcmp(argv[1], "sleep") == 0) {
        /* A non-interactive shell starts a background job with SIGQUIT
         * ignored (POSIX); this one is meant to die of it. */
        signal(SIGQUIT, SIG_DFL);
        marker = 0x5a5a1234;
        sleep(300);
    } else if (argc > 1 && strcmp(argv[1], "inherit") == 0) {
        /* SIGQUIT as the shell left it -- ignored, for a background
         * job -- across exec. Still ignored, so the sleep is whole. */
        unsigned left = sleep(argc > 2 ? (unsigned)atoi(argv[2]) : 4);

        printf("slept, %u left\n", left);
        return left ? 2 : 0;
    } else if (argc > 1 && strcmp(argv[1], "ignore") == 0) {
        /* ... and this one to sleep through it: an ignored signal is
         * thrown away when sent, and must not end a sleep. */
        unsigned left;

        signal(SIGQUIT, SIG_IGN);
        left = sleep(4);
        printf("slept, %u left\n", left);
        return left ? 2 : 0;
    } else if (argc > 2 && strcmp(argv[1], "wait") == 0) {
        int st = 0;
        pid_t p = fork();

        if (p == 0) {
            execl(argv[0], argv[0], argv[2], (char *)0);
            _exit(127);
        }
        waitpid(p, &st, 0);
        /* 0x80 is Linux's WCOREDUMP; picolibc's <sys/wait.h> has no
         * macro for it yet. */
        printf("signal=%d core=%d\n", WIFSIGNALED(st) ? WTERMSIG(st) : 0,
               (st & 0x80) != 0);
        return 0;
    }
    return 1;
}
