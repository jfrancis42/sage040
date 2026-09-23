/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pty.h - pseudo-terminals. See pty.c.
 *
 * /dev/ptmx allocates a pair and gives back the master; the slave is
 * /dev/pts/N, and N comes from the master's TIOCGPTN, which is what
 * ptsname(3) asks.
 */
#ifndef PTY_H
#define PTY_H

#include "kernel.h"

struct file;

int  pty_init(void);            /* registers /dev/ptmx               */

/* Opening /dev/ptmx: allocate a pair, point `f` at the master. Called
 * by vfs.c, which recognises the device by name. */
int  pty_open_master(struct file *f);

/* A slave was opened. vfs.c calls this so the master can tell when the
 * program on the terminal has gone. */
void pty_slave_opened(void *priv);

/* Is this device one of the slaves? vfs.c asks before calling the
 * hook above, and nothing else needs to know. */
int  pty_is_slave(const char *name);

#endif /* PTY_H */
