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
 * themselves are in syscall.c.  Programs DO run unprivileged -- this
 * comment used to say the day was still coming -- and the promise it
 * made held: everything above the kernel already went through the gate
 * rather than around it, so nothing above had to change when it did.
 *
 * An access fault from user mode is first offered to vm_fault(), which
 * may make the page (demand paging, task 21): then the handler returns,
 * and the 68040 runs the faulting instruction again. A fault it cannot
 * resolve kills the program (SIGSEGV) and leaves the machine running; a
 * fault in supervisor mode panics, because there is nothing else it
 * could safely do.
 */
#include "kernel.h"
#include "console.h"
#include "task.h"
#include "signal.h"
#include "vm.h"
#include "errno.h"
#include "uapi.h"

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

/*
 * Did this come from user mode?
 *
 * The saved SR is the first word of every frame format, and bit 13 is
 * the supervisor bit. If it is clear the exception happened in a
 * program, and a program's mistake is not the kernel's to die of.
 */
#define SR_SUPERVISOR   0x2000

static int from_user(const u16 *f)
{
    return (f[0] & SR_SUPERVISOR) == 0;
}

/*
 * The address that could not be reached.
 *
 * Only meaningful in a format 7 frame, which is what the 68040 pushes
 * for an access fault. Offset 0x14 from the start of the frame, per the
 * layout in chapter 8 -- and worth taking from the frame rather than
 * from a register, because the register that held it has usually been
 * reused by the time anything reads it.
 */
static u32 fault_address(const u16 *f)
{
    return ((u32)f[10] << 16) | (u32)f[11];
}

/*
 * The special status word of a format 7 frame, and the bits of it that
 * matter here. RW is set for a read. WBnS are the 68040's pending
 * write-backs: a fault can leave up to three writes the processor had
 * accepted but not done, and the handler must do them before returning
 * (chapter 8). QEMU never leaves any -- it re-runs the whole instruction
 * -- so they are checked rather than emulated, and a frame that has one
 * is reported rather than resumed with a write silently lost.
 */
#define SSW_RW          0x0100
#define WB_VALID        0x0080

static u16 fault_ssw(const u16 *f)
{
    return f[6];
}

static int writebacks_pending(const u16 *f)
{
    return (f[7] | f[8] | f[9]) & WB_VALID;
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
 * exception frame. An access fault from a program may be resolved --
 * demand paging, below -- and then this returns and the instruction runs
 * again. Anything else reports as much as it can and stops -- a silent
 * hang is the one outcome worth ruling out.
 */
void exception_handler(const u32 *regs, const u16 *frame)
{
    unsigned vec = (unsigned)((frame[3] & 0x0fff) >> 2);
    unsigned fmt = (unsigned)(frame[3] >> 12);
    int i;

    /*
     * A fault in a program kills the program, not the machine.
     *
     * This is the first thing the MMU actually buys, and it only works
     * because the exception came from user mode: the kernel is intact,
     * its stack is its own, and the only thing that has to go is the
     * address space of whatever ran off the end of itself.
     *
     * Note what is NOT done here: returning. The 68040 pushes the
     * address of the FAULTING INSTRUCTION, so an rte re-runs it -- which
     * is what the demand paging above relies on, and why a fault it
     * could not resolve must end the program here, not return and fault
     * again forever.
     */
    /*
     * DEMAND PAGING. The page may be lazy, in the swap file, or shared
     * copy-on-write: vm_fault() makes it what the access needs, and
     * returning re-runs the instruction.
     *
     * One answer can loop: VM_FAULT_NOCHANGE, a page the tables say is
     * there and accessible, which is taken to be a stale translation and
     * flushed. If the MMU faults on it again regardless, something the
     * tables do not show is wrong -- a bus error, say -- so three of
     * those in a row, at one address, from one instruction, are treated
     * as the real fault they are. A fault that CHANGED something always
     * makes progress, and resets the count: counting those instead once
     * killed a program for making, in a fresh process in a reused task
     * slot, the same first-touch fault its predecessor had made.
     */
    if (vec == 2 && fmt == 7 && from_user(frame) && current && current->as &&
        !writebacks_pending(frame)) {
        static u32 last_addr, last_pc, nochange;
        u32 addr = fault_address(frame);
        int write = !(fault_ssw(frame) & SSW_RW);
        int r = vm_fault(current->as, addr, write);

        if (r == 0) {
            nochange = 0;
            return;
        }
        if (r == VM_FAULT_NOCHANGE) {
            if (addr == last_addr && frame_pc(frame) == last_pc) {
                nochange++;
            } else {
                nochange = 1;
            }
            last_addr = addr;
            last_pc = frame_pc(frame);
            if (nochange < 3) {
                return;
            }
            nochange = 0;
        }
        if (r == -ENOMEM) {
            /* Linux's answer to a page that cannot be had: SIGKILL. */
            kputs("\nout of memory at 0x");
            kputhex32(addr);
            kputs(": killed\n");
            current->signalled = SIGKILL;
            task_exit(128 + SIGKILL);
        }
    }

    if (from_user(frame) && current && current->as) {
        kputs("\n");
        kputs(exception_name(vec));
        if (fmt == 7) {
            kputs(" at 0x");
            kputhex32(fault_address(frame));
        }
        kputs(", pc=0x");
        kputhex32(frame_pc(frame));
        kputln("");
        /*
         * 139 is what a shell reports for a program killed by a
         * segmentation fault: 128 plus the signal number. Nothing
         * catches signals here, but the number a person sees should
         * still be the number they would see anywhere else.
         */
        /*
         * Its own fault ends its own task and nothing else. The kernel
         * is intact -- the exception came from user mode, so its stack
         * is its own -- and the only thing that has to go is the address
         * space of whatever ran off the end of itself.
         */
        current->signalled = SIGSEGV;
        task_exit(128 + SIGSEGV);
    }

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

struct pt_regs *irq_regs;

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
