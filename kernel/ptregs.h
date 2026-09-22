/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ptregs.h - what is on the kernel stack when user mode is interrupted.
 *
 * Every way into the kernel -- a system call, a device interrupt, an
 * exception -- saves all fifteen general registers with one movem and
 * then finds the processor's own exception frame directly above them.
 * That makes this one layout for all three, which is the point: a
 * signal is delivered by rewriting it, and a signal can be delivered on
 * the way out of any of them.
 *
 *   0   d0-d7          saved by the stub
 *  32   a0-a6          saved by the stub
 *  60   sr             } pushed by the processor
 *  62   pc             }
 *  66   format/vector  }
 *
 * The user stack pointer is not here. The 68040 keeps it in a register
 * of its own, which switch_context() carries between tasks, so it is
 * read and written with `move usp` -- see signal.c.
 *
 * For a system call the d0 slot holds the call NUMBER until the call
 * finishes and its result is stored over it. That is what makes a
 * restart possible: put the number back, step the pc back over the
 * trap, and the same call runs again with the same arguments.
 */
#ifndef PTREGS_H
#define PTREGS_H

#include "kernel.h"

struct pt_regs {
    u32 d[8];
    u32 a[7];
    u16 sr;
    u32 pc;
    u16 format;                 /* format in the top 4 bits, vector below */
} __attribute__((packed));

#define PT_SR_SUPER     0x2000

/*
 * The registers of whatever the current device interrupt interrupted,
 * or null outside one. Set by mfp_dispatch() for the length of a
 * handler; the timer reads it to charge a tick to user or system time.
 */
extern struct pt_regs *irq_regs;

/* Did the interrupted code run in user mode? */
static inline int pt_user_mode(const struct pt_regs *r)
{
    return (r->sr & PT_SR_SUPER) == 0;
}

#endif /* PTREGS_H */
