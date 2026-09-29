/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * wb040.c - the MC68040's pending write-backs, completed in software.
 *
 * Reference: MC68040 User's Manual, 8.4.6 (the access error stack
 * frame) and Table 8-6.
 *
 * The 68040 retires an instruction before its writes reach memory: they
 * wait in up to three write-back registers. When an access faults, the
 * access error frame (format $7) holds whatever was still waiting --
 * address, data, and a status word each -- and the chip does NOT do
 * them on the rte: "the RTE reads only the stack information from
 * offset $0-$D". The handler must. A store to a page that is not
 * present is exactly this: the instruction has executed, its write sits
 * in WB2, and the stacked PC is past it; re-running nothing, the write
 * has to be made here or it is lost.
 *
 * QEMU re-runs the whole faulting instruction instead and never leaves
 * a write-back, so NOTHING HERE CAN BE EXERCISED UNDER QEMU. It is
 * written from the manual, checked by inspection, and first runs on a
 * real 68040 -- which, until it existed, would have killed a program at
 * its first write to any demand-paged page.
 *
 * The frame, as the u16s trap.c passes around:
 *   f[6]  SSW       f[7]  WB3S    f[8]  WB2S    f[9]  WB1S
 *   f[10] FA        f[12] WB3A    f[14] WB3D    f[16] WB2A
 *   f[18] WB2D      f[20] WB1A    f[22] WB1D/PD0 ... f[28] PD3
 */
#include "kernel.h"
#include "uaccess.h"
#include "wb040.h"

#define WBS_VALID   0x0080
#define WBS_SIZE(s) (((s) >> 5) & 3)    /* 0 long, 1 byte, 2 word, 3 line */
#define WBS_TT(s)   (((s) >> 3) & 3)    /* 1 = MOVE16                     */
#define WBS_TM(s)   ((s) & 7)           /* 1 user data, 5 supervisor data */

#define SSW_RW      0x0100
#define SSW_TT(s)   (((s) >> 3) & 3)
#define SSW_TM(s)   ((s) & 7)

static u32 frame_long(const u16 *f, int word)
{
    return ((u32)f[word] << 16) | f[word + 1];
}

/*
 * One write. `data` as the frame holds it: WB2D and WB3D are register
 * aligned (a byte in bits 7-0, a word in 15-0), WB1D memory aligned --
 * each byte in the lane it would drive onto the bus, which puts a long
 * written to an address ending in 01 as bits 23-0 then 31-24 (Table
 * 8-5). Rotating left by eight bits per unit of misalignment turns the
 * second into the first; after that the value's top bytes are the ones
 * to write.
 */
static int one_write(u32 addr, u32 data, u16 status, int memaligned)
{
    unsigned size = WBS_SIZE(status);
    u32 n = size == 1 ? 1 : size == 2 ? 2 : 4;
    u8 buf[4];

    if (size == 3) {
        return -1;              /* a line: only a push, handled apart */
    }
    if (memaligned) {
        unsigned rot = (addr & 3) * 8;

        data = rot ? (data << rot) | (data >> (32 - rot)) : data;
        data >>= (4 - n) * 8;   /* the leading n bytes, now low */
    }
    buf[0] = (u8)(data >> ((n - 1) * 8));
    if (n > 1) buf[1] = (u8)(data >> ((n - 2) * 8));
    if (n > 2) buf[2] = (u8)(data >> 8);
    if (n > 3) buf[3] = (u8)data;

    if (WBS_TM(status) == 5) {
        /* A supervisor data write: the kernel's own, and its memory is
         * identity-mapped, so it is simply done. */
        u8 *p = (u8 *)addr;
        u32 i;

        for (i = 0; i < n; i++) {
            p[i] = buf[i];
        }
        return 0;
    }
    return copy_to_user(addr, buf, n) < 0 ? -1 : 0;
}

static void keep(struct fault_wb *u, u32 addr, u32 data, u16 status,
                 int memaligned)
{
    if (!u || u->n >= 3) {
        return;
    }
    u->magic = FAULT_WB_MAGIC;
    u->w[u->n].addr = addr;
    u->w[u->n].data = data;
    u->w[u->n].status = status;
    u->w[u->n].memaligned = (u16)memaligned;
    u->n++;
}

int wb040_push_fault(const u16 *f)
{
    u16 ssw = f[6];

    return !(ssw & SSW_RW) && SSW_TT(ssw) == 0 && SSW_TM(ssw) == 0;
}

int wb040_complete(const u16 *f, struct fault_wb *undone)
{
    static const struct {
        int status, addr, data, memaligned;
    } wb[3] = {
        { 9, 20, 22, 1 },       /* WB1: the bus controller's */
        { 8, 16, 18, 0 },       /* WB2: the data memory unit's */
        { 7, 12, 14, 0 },       /* WB3: the integer unit's */
    };
    int i, failed = 0;

    for (i = 0; i < 3; i++) {
        u16 status = f[wb[i].status];
        u32 addr, data;

        if (!(status & WBS_VALID)) {
            continue;
        }
        /* A MOVE16 is restarted rather than completed: its read is
         * repeated, which for ordinary memory is harmless (Table 8-6's
         * "easy cleanup"). And WB1 of a push fault is the push. */
        if (WBS_TT(status) == 1 || (i == 0 && wb040_push_fault(f))) {
            continue;
        }
        addr = frame_long(f, wb[i].addr);
        data = frame_long(f, wb[i].data);
        if (one_write(addr, data, status, wb[i].memaligned) < 0) {
            keep(undone, addr, data, status, wb[i].memaligned);
            failed++;
        }
    }
    return failed;
}

int wb040_redo(const struct fault_wb *w)
{
    u32 i;

    if (w->magic != FAULT_WB_MAGIC) {
        return 0;
    }
    for (i = 0; i < w->n && i < 3; i++) {
        if (one_write(w->w[i].addr, w->w[i].data, w->w[i].status,
                      w->w[i].memaligned) < 0) {
            return -1;
        }
    }
    return 0;
}
