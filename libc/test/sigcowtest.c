/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sigcowtest - a signal that arrives while the program is taking a page
 * fault the kernel resolves.
 *
 * A caught SIGCHLD used to kill the program when it landed during a
 * copy-on-write (or demand-paging) fault: the way back to the program
 * went through signal delivery with the fault's format-7 frame still in
 * place, the kernel refused to build a handler frame on it, and made
 * that a SIGSEGV. bash died that way on every mistyped command at the
 * machine's real speed -- its child ("command not found") exits at once,
 * and the SIGCHLD met bash touching pages its fork had just shared.
 *
 * So: fork children that exit at once, and meanwhile write to pages the
 * fork made copy-on-write, over and over, with a SIGCHLD handler. The
 * program surviving, and the handler having run once per child, is the
 * pass.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ROUNDS  300
#define PAGES   48
#define PG      4096

static volatile int caught;

static void on_chld(int sig)
{
    (void)sig;
    while (waitpid(-1, 0, WNOHANG) > 0) {
        caught++;
    }
}

int main(void)
{
    static char mem[PAGES * PG];
    struct sigaction sa;
    int r, p;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, 0);
    memset(mem, 1, sizeof(mem));

    for (r = 0; r < ROUNDS; r++) {
        pid_t pid = fork();

        if (pid == 0) {
            _exit(0);
        }
        /* Every page is copy-on-write now: each store below faults. */
        for (p = 0; p < PAGES; p++) {
            mem[p * PG + (r % PG)] = (char)r;
        }
    }
    /* Not pause(): a SIGCHLD handled just before it would leave it
     * waiting for one that already came. */
    for (p = 0; caught < ROUNDS && p < 1000; p++) {
        usleep(10000);
    }
    printf("sigcowtest: %d children, %d SIGCHLDs handled, alive\n", ROUNDS,
           caught);
    return caught == ROUNDS ? 0 : 1;
}
