/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tty.h - the terminal, above the devices that carry it.
 *
 * /dev/console and /dev/tty are this, not a UART. A terminal is a line
 * discipline and a set of places characters come from and go to; a UART
 * is one of those places, and so is a screen.
 *
 * The split matters because this machine has more than one of each. A
 * character can arrive from the serial port or from a keyboard, and it
 * should appear on the screen and in the serial log both. Keeping the
 * line discipline inside the UART driver made that impossible to say --
 * echo went out of the same chip the character came in on, so with the
 * console on the framebuffer you typed blind.
 *
 * SOURCES AND SINKS. Any number of each. Input is polled from every
 * source; output goes to every enabled sink. Nothing here knows what
 * kind of device any of them is: they are `struct chardev`, and a source
 * is one whose ioctl answers FIONREAD.
 *
 * Kernel messages go through here too, which is what makes them appear
 * on every sink -- including the serial line, always, whatever the
 * screen is doing. That is the whole point: the log is never lost
 * because the display was in an interesting state.
 */
#ifndef TTY_H
#define TTY_H

#include "kernel.h"

struct chardev;

#define TTY_MAX_SOURCES  4
#define TTY_MAX_SINKS    4

/* Register /dev/console and /dev/tty, and bind descriptors 0, 1 and 2. */
int  tty_init(void);

/* Somewhere characters can come from, or go to. A device may be both --
 * a serial port usually is. */
int  tty_add_source(struct chardev *d);
int  tty_add_sink(struct chardev *d);

/* Turn one sink off without removing it, which is what the shell's
 * `console` command does. A source cannot be turned off: silently
 * ignoring a keystroke somebody typed is never the helpful answer. */
int  tty_sink_enable(const char *name, int on);

/* Walk the sinks, for anything that wants to report them. */
struct chardev *tty_sink(int index, int *enabled);
struct chardev *tty_source(int index);

struct chardev *tty_device(void);

#endif /* TTY_H */
