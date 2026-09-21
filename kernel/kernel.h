/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * kernel.h - the Sage040 kernel's own interfaces.
 *
 * Hardware register definitions come from ../tests/sage040.h, which is
 * shared with the device tests and the boot ROM.  Everything declared
 * here is kernel code and runs in supervisor mode.
 */
#ifndef KERNEL_H
#define KERNEL_H

#include "sage040.h"

/*
 * Version.  Bump the minor number for anything that changes behaviour a
 * program could notice, and the major number when the system call
 * numbers or the on-disk layout change.  It is printed at every boot so
 * that a screenshot or a captured serial log says which kernel produced
 * it -- with several builds on the disk at once, that stops being
 * obvious immediately.
 */
#define KERNEL_NAME           "Sage040"
#define KERNEL_VERSION_MAJOR  0
#define KERNEL_VERSION_MINOR  1
#define KERNEL_VERSION        "0.1"

/* Defined once, in version.c -- see the comment there. */
extern const char kernel_version[];
extern const char kernel_build[];

/* ---------------------------------------------------------------- */
/* Console (console.c)                                               */
/*                                                                    */
/* The kernel's only terminal: the NS16550A at 0xff000000.  Output is */
/* polled, and so is input; an interrupt-driven receive path is a     */
/* later change and will not alter these signatures.                  */
/* ---------------------------------------------------------------- */
void con_init(void);

void kputc(char c);                    /* one character; LF -> CR LF  */
void kputs(const char *s);             /* a string, no newline added  */
void kputln(const char *s);            /* a string and a newline      */

int  kgetc(void);                      /* block for one character     */
int  kgetc_nb(void);                   /* -1 if nothing is waiting    */
int  kgets(char *buf, int size);       /* a line, echoed and editable */

/* Number formatting, used by the startup messages. */
void kputhex8(u8 v);
void kputhex16(u16 v);
void kputhex32(u32 v);
void kputdec(u32 v);

/* ---------------------------------------------------------------- */
/* Traps and exceptions (trap.c)                                     */
/* ---------------------------------------------------------------- */
void trap_init(void);                  /* fill in the vector table    */
void panic(const char *msg) __attribute__((noreturn));

/* System calls, reached by user programs with TRAP #0.  The call
 * number goes in d0, the argument in d1, and the result comes back
 * in d0.  See syscall.c. */
#define SYS_PUTC    0
#define SYS_PUTS    1
#define SYS_GETC    2
#define SYS_GETS    3
#define SYS_NCALLS  4

s32 syscall_dispatch(u32 nr, u32 arg1, u32 arg2);
s32 syscall(u32 nr, u32 arg1, u32 arg2);   /* makes the TRAP #0 call */

/* ---------------------------------------------------------------- */
/* The console shell (shell.c)                                       */
/* ---------------------------------------------------------------- */
void shell(void) __attribute__((noreturn));

/* ---------------------------------------------------------------- */
/* Startup hardware inventory (probe.c)                              */
/* ---------------------------------------------------------------- */
void probe_all(void);
u32  probe_memory(void);               /* bytes of contiguous RAM     */

/* Linker-provided symbols describing the kernel image. */
extern char _vectors[], _end[], _stack_top[];

/* Stop the machine.  Defined in start.s. */
void halt(void) __attribute__((noreturn));

#endif /* KERNEL_H */
