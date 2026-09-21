/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fb.h - the framebuffer as a device programs can open.
 *
 * Registers /dev/fb0 as a character device whose ioctls draw. The
 * drawing itself belongs to whichever struct fbdev was registered; this
 * layer validates arguments, fills in what a driver did not implement,
 * and is the only thing a program ever talks to.
 */
#ifndef FB_H
#define FB_H

#include "kernel.h"

/* Bring up /dev/fb0 over the first registered framebuffer. */
int fb_init(void);

#endif /* FB_H */
