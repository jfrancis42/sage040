/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ctty.c - sessions and controlling terminals.
 *
 * A SESSION is what a login is: setsid() starts one, and its leader is
 * the first program of the login. A session has at most one
 * CONTROLLING TERMINAL, and a terminal controls at most one session.
 * That pairing is what lets the terminal reach the whole login when it
 * matters: hanging up -- an ssh connection dropped, a pty's master
 * closed -- sends SIGHUP to the session's leader and its foreground
 * job, which is how a shell learns nobody is there any more; and
 * /dev/tty is the terminal of whoever opens it, which is how a program
 * whose stdin is a pipe asks a person a question anyway.
 *
 * Before this the machine had sessions in name only: setsid() worked
 * and nothing called it, so every console process was in the boot
 * task's session and every ssh shell in Dropbear's; no terminal knew
 * which session it served, TIOCSCTTY was "Invalid argument", there was
 * no /dev/tty, and a dropped ssh connection hung up nobody.
 *
 * The rules are Linux's:
 *
 *   - a session leader with no controlling terminal that opens a
 *     terminal nobody controls gets it, unless it says O_NOCTTY;
 *   - TIOCSCTTY asks for it explicitly; root, with argument 1, may take
 *     a terminal another session has;
 *   - TIOCNOTTY gives it up -- for a leader, the whole session loses it
 *     and its foreground job is hung up;
 *   - setsid() starts a session with no terminal;
 *   - when a session leader exits, its terminal is released and the
 *     foreground job hung up, so nothing is left reading a terminal
 *     that belongs to nobody;
 *   - fork inherits the terminal, exec keeps it.
 *
 * KERNEL TASKS are outside all of this: the console's own shell is a
 * kernel task, holds no controlling terminal, and keeps the authority
 * it always had to hand its terminal to a job (TIOCSPGRP, jobctl).
 */
#include "kernel.h"
#include "ctty.h"
#include "dev.h"
#include "task.h"
#include "signal.h"
#include "errno.h"

/* The task leading session `sid`, or 0. */
static struct task *leader_of(int sid)
{
    struct task *t = task_find(sid);

    return (t && t->sid == sid && t->state != TASK_ZOMBIE) ? t : 0;
}

/* Every task in the session loses the terminal; the terminal loses the
 * session. With `hangup`, its foreground job is sent SIGHUP and SIGCONT
 * first (SIGCONT so that a stopped job wakes to hear it). */
static void detach_session(struct ttyctl *tc, int hangup)
{
    struct task *t;
    int i, sid = tc->sid;

    if (hangup && tc->pgrp > 0) {
        signal_group(tc->pgrp, SIGHUP);
        signal_group(tc->pgrp, SIGCONT);
    }
    for (i = 0; (t = task_nth(i)) != 0; i++) {
        if (t->ctty == tc && (sid == 0 || t->sid == sid)) {
            t->ctty = 0;
        }
    }
    tc->sid = 0;
    tc->pgrp = 0;
}

static void attach(struct ttyctl *tc)
{
    tc->sid = current->sid;
    tc->pgrp = current->pgid;
    current->ctty = tc;
}

void ctty_opened(struct chardev *cd, int flags)
{
    struct task *c = current;

    if (!cd || !cd->tc || (flags & O_NOCTTY) || !c || !c->as) {
        return;
    }
    if (c->pid == c->sid && !c->ctty && cd->tc->sid == 0) {
        attach(cd->tc);
    }
}

int ctty_ioctl(struct ttyctl *tc, u32 req, u32 arg)
{
    struct task *c = current;

    switch (req) {
    case TIOCSCTTY:
        if (!c) {
            return -EPERM;
        }
        if (c->ctty == tc && tc->sid == c->sid) {
            return 0;                       /* it already is */
        }
        if (c->pid != c->sid || c->ctty) {
            return -EPERM;                  /* not a leader, or has one */
        }
        if (tc->sid != 0 && tc->sid != c->sid) {
            if (arg != 1 || c->euid != 0) {
                return -EPERM;
            }
            detach_session(tc, 0);          /* root takes it */
        }
        attach(tc);
        return 0;

    case TIOCNOTTY:
        if (!c || c->ctty != tc) {
            return -ENOTTY;
        }
        if (c->pid == c->sid) {
            detach_session(tc, 1);
        } else {
            c->ctty = 0;
        }
        return 0;

    case TIOCGSID:
        if (!arg) {
            return -EINVAL;
        }
        if (tc->sid == 0) {
            return -ENOTTY;
        }
        *(int *)arg = tc->sid;
        return 0;
    }
    return CTTY_NOT_MINE;
}

int ctty_may_setpgrp(struct ttyctl *tc, int pgrp)
{
    struct task *c = current, *t;
    int i;

    if (!c || !c->as || tc->sid == 0) {
        return 0;                           /* kernel task, or no session */
    }
    if (c->ctty != tc || c->sid != tc->sid) {
        return -ENOTTY;                     /* not its controlling terminal */
    }
    for (i = 0; (t = task_nth(i)) != 0; i++) {
        if (t->pgid == pgrp && t->state != TASK_ZOMBIE) {
            return t->sid == tc->sid ? 0 : -EPERM;
        }
    }
    return -EPERM;
}

void ctty_hangup(struct ttyctl *tc)
{
    struct task *l;

    if (!tc || tc->sid == 0) {
        return;
    }
    l = leader_of(tc->sid);
    if (l) {
        signal_send(l, SIGHUP);
        signal_send(l, SIGCONT);
    }
    detach_session(tc, 1);
}

void ctty_exiting(struct task *t)
{
    struct ttyctl *tc = t->ctty;

    if (tc && t->pid == t->sid && tc->sid == t->sid) {
        detach_session(tc, 1);
    }
    t->ctty = 0;
}

void ctty_setsid(void)
{
    if (current) {
        current->ctty = 0;
    }
}

struct chardev *ctty_device(void)
{
    struct task *c = current;

    return (c && c->ctty && c->ctty->sid == c->sid) ? c->ctty->dev : 0;
}

u32 ctty_number(struct task *t)
{
    if (!t || !t->ctty || !t->ctty->dev || t->ctty->sid != t->sid) {
        return 0;
    }
    return t->ctty->dev->rdev;
}
