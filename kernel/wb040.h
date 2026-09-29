/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * wb040.h - the MC68040's pending write-backs, completed in software.
 *
 * See wb040.c.
 */
#ifndef WB040_H
#define WB040_H

#include "types.h"

/*
 * Writes a fault left undone. Kept in the task between the fault and
 * the signal frame, and carried in the frame itself -- so that a
 * handler that returns gets them done by sigreturn, and one that
 * longjmps away abandons them with the rest of that path.
 */
#define FAULT_WB_MAGIC  0x57423034UL    /* "WB04" */

struct fault_wb {
    u32 magic;                  /* FAULT_WB_MAGIC when there are any  */
    u32 n;
    struct {
        u32 addr;
        u32 data;
        u16 status;             /* the frame's WBnS                   */
        u16 memaligned;         /* WB1's data is laid out as on the bus */
    } w[3];
} __attribute__((packed));

/*
 * Do the pending write-backs of an access error frame (format $7), in
 * the order the manual requires: WB1, WB2, WB3. Those that cannot be
 * done -- their address faults too -- are added to `undone` (which may
 * be null). Returns how many could not be done. A write the frame marks
 * as the kernel's (supervisor data) goes straight to memory.
 */
int wb040_complete(const u16 *frame, struct fault_wb *undone);

/* Do writes carried in a signal frame. -1 if any still cannot be. */
int wb040_redo(const struct fault_wb *w);

/* A cache line push faulted: a physical bus error on the push itself,
 * which no amount of software can finish. */
int wb040_push_fault(const u16 *frame);

#endif /* WB040_H */
