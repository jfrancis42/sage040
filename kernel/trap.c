/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * trap.c - exception dispatch and the system call gate.
 *
 * Reference: Motorola M68040 User's Manual, chapter 8 (exception
 * processing).
 *
 * Every vector except reset points at _exc_common in start.s, which
 * saves the registers and calls exception_handler() below.  A 68040
 * pushes a format/vector word on every exception, so the handler can
 * identify itself from its own stack frame instead of needing 255
 * separate stubs.
 *
 * TRAP #0 is redirected to _trap0_entry, the system call gate; the calls
 * themselves are in syscall.c.  Nothing runs unprivileged yet, but
 * everything above the kernel already goes through the gate rather than
 * around it, so the day something does, the code above does not change.
 */
#include "kernel.h"
#include "console.h"

#define VEC_TRAP0   32          /* vectors 32..47 are TRAP #0..#15 */

/*
 * The exception frame, read a word at a time.
 *
 * All 68040 stack frame formats begin with the same three items, so this
 * much is safe whatever kind of exception arrived:
 *
 *   word 0      status register
 *   words 1,2   program counter
 *   word 3      format (bits 15..12) and vector offset (bits 11..0)
 *
 * Reading through a u16 pointer rather than a struct keeps it free of
 * any assumption about how the compiler lays out a mixed-width struct.
 */
static u32 frame_pc(const u16 *f)
{
    return ((u32)f[1] << 16) | (u32)f[2];
}

static const char *exception_name(unsigned vec)
{
    switch (vec) {
    case 2:  return "bus error";
    case 3:  return "address error";
    case 4:  return "illegal instruction";
    case 5:  return "divide by zero";
    case 6:  return "CHK instruction";
    case 7:  return "TRAPcc";
    case 8:  return "privilege violation";
    case 9:  return "trace";
    case 10: return "line 1010 emulator";
    case 11: return "line 1111 emulator";
    case 14: return "format error";
    case 15: return "uninitialised interrupt";
    case 24: return "spurious interrupt";
    case 48: return "FP branch on unordered";
    case 49: return "FP inexact result";
    case 50: return "FP divide by zero";
    case 51: return "FP underflow";
    case 52: return "FP operand error";
    case 53: return "FP overflow";
    case 54: return "FP signalling NaN";
    case 55: return "FP unimplemented data type";
    default:
        if (vec >= 25 && vec <= 31) return "autovector interrupt";
        if (vec >= 32 && vec <= 47) return "TRAP instruction";
        if (vec >= 64)              return "user interrupt vector";
        return "unknown";
    }
}

static const char *regnames[15] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6"
};

/*
 * Called from _exc_common with the fifteen saved registers and the
 * exception frame.  Nothing here is recoverable yet, so it reports as
 * much as it can and stops -- a silent hang is the one outcome worth
 * ruling out.
 */
void exception_handler(const u32 *regs, const u16 *frame)
{
    unsigned vec = (unsigned)((frame[3] & 0x0fff) >> 2);
    unsigned fmt = (unsigned)(frame[3] >> 12);
    int i;

    kputs("\n*** exception ");
    kputdec(vec);
    kputs(": ");
    kputs(exception_name(vec));
    kputs("\n    pc=0x");
    kputhex32(frame_pc(frame));
    kputs("  sr=0x");
    kputhex16(frame[0]);
    kputs("  frame format ");
    kputdec(fmt);
    kputc('\n');

    for (i = 0; i < 15; i++) {
        kputs((i % 4) == 0 ? "    " : "  ");
        kputs(regnames[i]);
        kputc('=');
        kputhex32(regs[i]);
        if ((i % 4) == 3) {
            kputc('\n');
        }
    }
    kputc('\n');

    panic("unhandled exception");
}

void panic(const char *msg)
{
    kputs("\n*** panic: ");
    kputs(msg);
    kputs("\n*** halted.\n");
    halt();
}

void trap_init(void)
{
    extern void _trap0_entry(void);
    u32 *vectors = (u32 *)_vectors;

    vectors[VEC_TRAP0] = (u32)_trap0_entry;
}
