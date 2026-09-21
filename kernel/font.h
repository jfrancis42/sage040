/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * font.h - the text font the framebuffer console draws with.
 *
 * One font, fixed width, no metrics and no kerning: this is a character
 * cell display, and a cell is a cell. See font8x16.c for where the
 * glyphs came from.
 */
#ifndef FONT_H
#define FONT_H

#include "kernel.h"

#define FONT_WIDTH   8
#define FONT_HEIGHT  16
#define FONT_GLYPHS  256

/* Glyph g, row r, most significant bit leftmost. */
extern const u8 font8x16[FONT_GLYPHS][FONT_HEIGHT];

#endif /* FONT_H */
