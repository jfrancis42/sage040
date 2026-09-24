/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * uptime - how long the machine has been up, and how busy.
 *
 *   14:53:33 up 2 days,  4:07,  3 users,  load average: 0.42, 0.19, 0.06
 *
 * The whole line is w(1)'s header, and it is printed by the same code
 * (print_status in whocommon.h) so the two cannot drift. The load
 * average is the interesting half: three exponentially-weighted moving
 * averages of the run queue -- over one, five and fifteen minutes -- so
 * a single number reads as a trend, not a snapshot. The kernel keeps
 * them (loadavg.c); this only asks and formats.
 *
 * `-p` ("pretty") prints just the duration in words, as uptime(1) does;
 * `-s` prints when the machine came up. Both are conveniences on top of
 * the same two facts the default line already carries.
 */
#include "whocommon.h"

static void pretty(u32 uptime)
{
    u32 days = uptime / 86400;
    u32 hours = uptime % 86400 / 3600;
    u32 mins = uptime % 3600 / 60;
    char n[16];
    int printed = 0;

    puts("up");
    if (days) {
        puts(" ");
        num(days, n);
        puts(n);
        puts(days == 1 ? " day" : " days");
        printed = 1;
    }
    if (hours) {
        puts(" ");
        num(hours, n);
        puts(n);
        puts(hours == 1 ? " hour" : " hours");
        printed = 1;
    }
    if (mins || !printed) {
        puts(" ");
        num(mins, n);
        puts(n);
        puts(mins == 1 ? " minute" : " minutes");
    }
    puts("\n");
}

int main(int argc, char **argv)
{
    struct sysinfo si;
    u32 now = (u32)time(0);
    u32 uptime = 0;
    int i, users = 0;

    memset(&si, 0, sizeof(si));
    if (sysinfo(&si) == 0) {
        uptime = si.uptime;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            pretty(uptime);
            return 0;
        }
        if (strcmp(argv[i], "-s") == 0) {
            /* When it came up: now minus how long it has been up. */
            char when[20];

            stamp(now > uptime ? now - uptime : 0, when);
            puts(when);
            puts("\n");
            return 0;
        }
        eputs("usage: uptime [-p | -s]\n");
        return 2;
    }

    who_load();
    for (i = 0; i < who_n; i++) {
        users += who_is_login(i);
    }
    print_status(now, uptime, users, si.loads);
    return 0;
}
