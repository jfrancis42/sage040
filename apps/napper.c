/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * napper - sleep, and prove the machine did not stop with us.
 *
 * nanosleep() used to loop on STOP in supervisor mode, which halted the
 * processor until the next tick and then went straight back to it.
 * Preemption only happens on the way back to user mode, so a task in
 * that loop was never preempted and nothing else ran for the whole
 * sleep. Run this in the background and anything else in the
 * foreground: if the foreground work finishes while this is still
 * sleeping, the sleep is yielding properly.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    u32 secs = 3;
    struct timespec ts;

    if (argc > 1) {
        secs = (u32)(argv[1][0] - '0');
    }

    puts("napper: sleeping\n");
    ts.tv_sec = secs;
    ts.tv_nsec = 0;
    nanosleep(&ts, 0);
    puts("napper: awake\n");
    return 0;
}
