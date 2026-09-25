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

/* Register the two terminals (/dev/tty1, /dev/console), point kernel
 * messages at the serial line, and bind the boot task's 0, 1 and 2. */
int  tty_init(void);

/* Bind the current task's 0, 1 and 2 to a named terminal ("tty1",
 * "console"): how a getty attaches to the line it serves. */
int  tty_attach(const char *name);

/*
 * Wire a raw device into its terminal. The keyboard and framebuffer
 * belong to the screen (tty1); the serial UART to the serial line
 * (console). A device may be both a source and a sink -- a UART is.
 */
int  tty_add_source(struct chardev *d);
int  tty_add_sink(struct chardev *d);

/*
 * The source's driver takes its receive interrupt and calls
 * tty_input_irq(dev) from it, passing its own device so the right
 * terminal is drained; the source is not polled any more.
 */
int  tty_source_irq(struct chardev *d);
void tty_input_irq(struct chardev *d);
u32  tty_overruns(void);

/* Walk the terminals' devices, for the boot banner: returns the device
 * and, through the arguments, which terminal it belongs to and whether
 * it is a source. 0 past the end. */
struct chardev *tty_nth(int index, const char **ttyname, int *is_source);

/* /dev/console: the serial line, where kernel messages go. */
struct chardev *tty_device(void);

/*
 * Look for an interrupt or stop character on every terminal, from the
 * timer tick -- the program running and reading nothing would otherwise
 * be impossible to interrupt -- and wake anything waiting for input.
 */
void tty_poll_signals(void);

/*
 * Hand the SERIAL terminal to a process group. Used by the boot task
 * as it starts the first getty; per-terminal handoff after that goes
 * through TIOCSPGRP on each terminal's own descriptor.
 */
void tty_set_foreground(int pid);

/* Canonical mode, echo on, signals on for the terminal a descriptor is
 * open on: the state a shell hands a program, put back after one that
 * changed it was killed before it could. */
void tty_reset_fd(int fd);

#endif /* TTY_H */
