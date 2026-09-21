/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fbcon.h - the text console on the framebuffer.
 *
 * Registers /dev/fbcon over whatever `struct fbdev` was registered. It
 * writes glyphs and reads nothing: input on this machine arrives on the
 * serial line, so a read of /dev/fbcon is handed to the serial terminal.
 */
#ifndef FBCON_H
#define FBCON_H

#include "kernel.h"

struct chardev;

int  fbcon_init(void);
void fbcon_clear(void);
int  fbcon_rows(void);
int  fbcon_cols(void);

/* Also send everything to the serial console, so a captured serial log
 * still has what the screen shows. */
int  fbcon_mirror(int on);

struct chardev *fbcon_device(void);

#endif /* FBCON_H */
