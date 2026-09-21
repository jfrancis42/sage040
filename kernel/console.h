/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * console.h - how the kernel itself prints.
 *
 * Separate from the system call path on purpose. These go straight to
 * the registered console device, without a descriptor, without the VFS
 * and without a trap, so that they still work when the thing being
 * reported is that one of those is broken. A panic that cannot print is
 * a panic nobody can diagnose.
 *
 * Programs do not use these. Programs call write(1, ...).
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include "kernel.h"
#include "dev.h"

/* Called once by the console driver as it registers itself. */
void console_set(struct chardev *d);
struct chardev *console_get(void);

void kputc(char c);
void kputs(const char *s);
void kputln(const char *s);
void kputhex8(u8 v);
void kputhex16(u16 v);
void kputhex32(u32 v);
void kputdec(u32 v);
void kput2(u32 v);              /* two digits, zero padded */

#endif /* CONSOLE_H */
