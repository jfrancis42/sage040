/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * kernel.h - the few things every part of the kernel needs.
 *
 * Hardware register definitions come from ../tests/sage040.h, which is
 * shared with the device tests and the boot ROM. Nothing outside
 * drivers/ should include it for any other reason: a chip's registers
 * are that driver's business.
 */
#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"
#include "sage040.h"

/*
 * Version. Bump the minor number for anything a program could notice,
 * and the major number when the system call numbers or the on-disk
 * layout change. It is printed at every boot so that a screenshot or a
 * captured serial log says which kernel produced it -- with several
 * builds on the disk at once, that stops being obvious immediately.
 */
#define KERNEL_NAME           "SuckOS"    /* the system */
#define MACHINE_NAME          "Sage040"   /* the machine it runs on */
#define KERNEL_VERSION_MAJOR  0
#define KERNEL_VERSION_MINOR  3
#define KERNEL_VERSION        "0.3"

/* Defined once, in version.c -- see the comment there. */
extern const char kernel_version[];
extern const char kernel_build[];

/* Exception reporting (trap.c). */
void trap_init(void);
void panic(const char *msg) __attribute__((noreturn));

/* The startup hardware inventory (probe.c). */
void probe_all(void);
u32  probe_memory(void);

/* The shell (shell.c). It reaches the kernel only through system calls. */
void shell(void) __attribute__((noreturn));
void shell_getty(void) __attribute__((noreturn));

/* Linker-provided symbols describing the kernel image. */
extern char _vectors[], _end[], _stack_top[];

/* Stop the machine. Defined in start.s. */
void halt(void) __attribute__((noreturn));

#endif /* KERNEL_H */
