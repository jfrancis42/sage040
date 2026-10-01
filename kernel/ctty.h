/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ctty.h - sessions and controlling terminals (ctty.c).
 */
#ifndef CTTY_H
#define CTTY_H

#include "kernel.h"

struct ttyctl;
struct chardev;
struct task;

/* A terminal was opened: a session leader without one may get it. */
void ctty_opened(struct chardev *cd, int flags);

/* TIOCSCTTY, TIOCNOTTY, TIOCGSID for any terminal; CTTY_NOT_MINE for
 * every other request, which the terminal handles itself. */
#define CTTY_NOT_MINE  1
int  ctty_ioctl(struct ttyctl *tc, u32 req, u32 arg);

/* May the current task make `pgrp` this terminal's foreground job? */
int  ctty_may_setpgrp(struct ttyctl *tc, int pgrp);

/* The terminal hung up (a pty's master closed). */
void ctty_hangup(struct ttyctl *tc);

/* A task is exiting / starting a session. */
void ctty_exiting(struct task *t);
void ctty_setsid(void);

/* The current task's controlling terminal as a device, or 0: /dev/tty. */
struct chardev *ctty_device(void);

/* Its device number for /proc/<pid>/stat's tty_nr, or 0. */
u32  ctty_number(struct task *t);

#endif
