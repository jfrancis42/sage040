/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * w - who is logged in, and what they are running.
 *
 * The same question who(1) asks, with a header and one more column.
 * Where the answer comes from, and why it is neither utmp nor the
 * session table, is written up at the head of who.c.
 *
 * WHAT THIS DOES NOT PRINT, and why saying so beats inventing it:
 *
 *   FROM   the host an ssh session came from. Dropbear is built
 *          --disable-utmp and execs the shell itself, so nothing on
 *          this machine is ever told the peer's address. A column of
 *          "-" would be honest but useless, so there is none.
 *   IDLE   how long since that terminal was last typed at. The tty
 *          layer keeps no last-input time, so this cannot be computed
 *          from anything that exists.
 *   JCPU   per-terminal processor time. Tasks are charged utime and
 *   PCPU   stime individually, but nothing groups them by terminal.
 *   load   a load average. The scheduler keeps no running mean.
 *
 * WHAT is THE MOST RECENTLY STARTED THING THE LOGIN SHELL HAS RUNNING,
 * found by ancestry -- a task is in the column if the login shell is
 * somewhere up its parent chain. Not quite "the foreground job": the
 * console's foreground process group lives in tty.c and a pseudo-
 * terminal has no equivalent, so for an ssh session there is nothing
 * exact to report. The newest descendant is right whenever somebody is
 * waiting at a prompt for one thing to finish, which is the case `w`
 * is asked about, and it is wrong for a job put in the background
 * before a later one started. The heading says WHAT rather than
 * pretending to more precision than that.
 *
 * Ancestry rather than "everything on the same terminal", because the
 * console carries tasks that belong to nobody sitting at it -- klogd
 * is backgrounded by /etc/rc and keeps the console open for life, and
 * reporting it as what the user is doing would be a plain lie.
 */
#include "whocommon.h"

int main(int argc, char **argv)
{
    struct sysinfo si;
    u32 now = (u32)time(0);
    u32 uptime = 0;
    int i, users = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") != 0) {   /* -h: no heading, as usual */
            eputs("usage: w [-h]\n");
            return 2;
        }
    }
    memset(&si, 0, sizeof(si));
    if (sysinfo(&si) == 0) {
        uptime = si.uptime;
    }
    who_load();

    /* Count the logins before printing, so the header can say how many
     * there are rather than promising a number it has not seen. */
    for (i = 0; i < who_n; i++) {
        users += who_is_login(i);
    }

    if (argc == 1) {
        /* The header IS the uptime line -- time, how long up, who is on,
         * and the load average -- shared with uptime(1) so the two can
         * never disagree. */
        print_status(now, uptime, users, si.loads);
        puts("USER      TTY       LOGIN@            WHAT\n");
    }

    for (i = 0; i < who_n; i++) {
        const struct who_info *wi = &who_tab[i];
        char name[32], when[20];
        const char *what;
        u32 best;
        int j;

        if (!who_is_login(i)) {
            continue;
        }
        stamp(who_login_time(wi->start, uptime, now), when);
        uid_name(wi->uid, name, (int)sizeof(name));

        /* The shell itself is the floor: a login with nothing else
         * running is sitting at its prompt, and that is what it is
         * doing. Its own argv[0] still carries login's '-', which
         * would be noise in this column, so it is dropped. */
        what = wi->name[0] ? wi->name + 1 : "-";
        best = wi->start;
        for (j = 0; j < who_n; j++) {
            const struct who_info *o = &who_tab[j];

            if (o->pid == wi->pid || o->start < best || !o->cmd[0]) {
                continue;
            }
            if (who_descends_from(o->ppid, wi->pid)) {
                best = o->start;
                what = o->cmd;
            }
        }

        pad(name, 10);
        pad(wi->tty[0] ? wi->tty : "-", 10);
        pad(when, 18);
        puts(what);
        puts("\n");
    }
    return 0;
}
