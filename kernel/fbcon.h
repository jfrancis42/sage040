/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fbcon.h - the text console on the framebuffer.
 *
 * Registers /dev/fbcon over whatever `struct fbdev` was registered, and
 * adds it to the terminal's output sinks. It writes glyphs and reads
 * nothing -- a screen is not an input device, and input has its own path
 * through tty.c.
 */
#ifndef FBCON_H
#define FBCON_H

#include "kernel.h"

struct chardev;

int  fbcon_init(void);
void fbcon_clear(void);
int  fbcon_rows(void);
int  fbcon_cols(void);

struct chardev *fbcon_device(void);

#endif /* FBCON_H */
