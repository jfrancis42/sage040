/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * who - who is logged in.
 *
 * WHERE THE ANSWER COMES FROM, and why it is neither of the two obvious
 * things. Both were tried, in this order.
 *
 * NOT UTMP. The usual arrangement is a file: login(1) appends a record,
 * who(1) reads it, and something removes it when the session ends. That
 * would have been wrong here, because this machine's ssh sessions come
 * from Dropbear, which is built --disable-utmp and execs the user's
 * shell itself without going near login -- so a utmp written only by
 * login would have listed the console and silently omitted every ssh
 * login. A `who` that leaves people out is worse than no `who`.
 *
 * NOT "A SESSION LEADER WITH A TERMINAL" either, which is the answer a
 * Unix that has sessions ought to be able to give, and which this one
 * cannot: NOTHING ON THIS MACHINE CALLS setsid(). The call exists, but
 * login is started by the console shell as a job of its own -- so it is
 * a process-group leader, and POSIX makes setsid() fail for one -- and
 * Dropbear's session handling was never ported. Every task on the
 * console therefore shares the session of the shell that booted, and
 * that rule reported the machine's own idle and netd tasks as two
 * logged-in roots while missing the person actually at the keyboard.
 * (Fixing the sessions themselves is the better answer and is on the
 * list; it is a change to how the console login is started, not to
 * this program, and `who` should be right either way.)
 *
 * SO: A LOGIN IS A LOGIN SHELL ON A TERMINAL -- a task whose argv[0]
 * begins with '-'. That convention is not decoration: login(1) builds
 * "-bash" deliberately (auth/login.c), and Dropbear builds the same
 * for an interactive session and a bare "bash" for `ssh host command`
 * (run_shell_command in its dbutil.c). So it separates a person at a
 * terminal from a remote command that is not one, on both paths, with
 * neither program having been asked to cooperate -- and unlike a file
 * it cannot go stale, because it IS the task.
 *
 * The kernel reports argv[0] verbatim and this program decides what it
 * means, rather than the kernel answering "is this a login": the rule
 * is policy and belongs out here with the other policy.
 *
 * The login TIME is the moment that shell started. The kernel reports
 * it as jiffies since boot rather than as a date, because the wall
 * clock can be set and jiffies cannot go backwards; the sum in
 * who_login_time() stays right across a `date` that moves the clock.
 */
#include "whocommon.h"

int main(int argc, char **argv)
{
    struct sysinfo si;
    u32 now = (u32)time(0);
    u32 uptime = 0;
    int i, all = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            all = 1;            /* every task, its parent and its
                                 * session: for seeing what the machine
                                 * thinks, and for debugging the rule
                                 * above when it is wrong again */
        } else if (strcmp(argv[i], "-H") != 0) {
            eputs("usage: who [-a]\n");
            return 2;
        }
    }
    if (sysinfo(&si) == 0) {
        uptime = si.uptime;
    }
    who_load();

    puts(all ? "USER      TTY       PID   PPID  SID   LOGIN@            CMD\n"
             : "USER      TTY       LOGIN@\n");

    for (i = 0; i < who_n; i++) {
        const struct who_info *wi = &who_tab[i];
        char name[32], when[20], n[16];

        if (!all && !who_is_login(i)) {
            continue;
        }
        stamp(who_login_time(wi->start, uptime, now), when);
        uid_name(wi->uid, name, (int)sizeof(name));

        pad(name, 10);
        pad(wi->tty[0] ? wi->tty : "-", 10);
        if (all) {
            num((u32)wi->pid, n);
            pad(n, 6);
            num((u32)wi->ppid, n);
            pad(n, 6);
            num((u32)wi->sid, n);
            pad(n, 6);
            pad(when, 18);
            puts(wi->cmd[0] ? wi->cmd : "-");
        } else {
            puts(when);
        }
        puts("\n");
    }
    return 0;
}
