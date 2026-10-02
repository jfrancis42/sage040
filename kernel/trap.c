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
 * may make the page (demand paging, task 21): then any writes the fault
 * left pending are done (wb040.c) and the handler returns. A fault that
 * cannot be resolved becomes a signal the program can catch -- SIGSEGV,
 * SIGBUS, SIGILL, SIGFPE, SIGTRAP -- or, with no handler, ends it and
 * leaves the machine running; a fault in supervisor mode panics,
 * because there is nothing else it could safely do.
 */
#include "kernel.h"
#include "console.h"
#include "coredump.h"
#include "task.h"
#include "signal.h"
#include "vm.h"
#include "errno.h"
#include "uapi.h"
#include "wb040.h"
#include "string.h"
#include "ptregs.h"
#include "klog.h"
#include "timer.h"

#define VEC_TRAP0   32          /* vectors 32..47 are TRAP #0..#15 */
#define VEC_TRAPCC  7           /* TRAPcc, TRAPV, CHK2: the stack limit */

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
 * matter here (Figure 8-7). RW is set for a read; ATC for a fault in
 * translation -- a page not there, or not allowed -- and clear for a
 * physical bus error. The frame also holds up to three writes the
 * processor had accepted but not done, which the handler must do before
 * returning: wb040.c.
 */
#define SSW_RW          0x0100
#define SSW_ATC         0x0400

static u16 fault_ssw(const u16 *f)
{
    return f[6];
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
 * exception_handler() is called from _exc_common with the fifteen saved
 * registers and the exception frame. An access fault from a program may
 * be resolved -- demand paging -- and then it returns and the program
 * carries on. A fault the program has a handler for becomes that signal.
 * Anything else reports as much as it can and stops -- a silent hang is
 * the one outcome worth ruling out. It returns how many bytes of the
 * frame _exc_common must drop (make_format0), usually none.
 */

/*
 * What a fault in a program is, as a signal: Linux/m68k's mapping.
 */
static int fault_signal(unsigned vec, int *code)
{
    switch (vec) {
    case 2:  *code = SEGV_MAPERR; return SIGSEGV;   /* refined by caller */
    case 3:  *code = BUS_ADRALN;  return SIGBUS;    /* address error      */
    case 5:  *code = FPE_INTDIV;  return SIGFPE;    /* divide by zero     */
    case 6:                                         /* CHK                */
    case 7:  *code = FPE_INTOVF;  return SIGFPE;    /* TRAPcc, TRAPV      */
    case 8:  *code = ILL_PRVOPC;  return SIGILL;    /* privilege          */
    case 9:  *code = TRAP_TRACE;  return SIGTRAP;
    case 47: *code = TRAP_BRKPT;  return SIGTRAP;   /* TRAP #15           */
    default:
        if (vec >= 33 && vec <= 46) {               /* TRAP #1..#14       */
            *code = ILL_ILLTRP;
            return SIGILL;
        }
        *code = ILL_ILLOPC;                          /* illegal, line A,   */
        return SIGILL;                               /* anything else      */
    }
}

/*
 * Will this program run a handler for `sig`? A fault is not a signal
 * anybody sent: it cannot be ignored or deferred, because the
 * instruction would only fault again. Blocked or ignored, it is taken
 * as its default -- the end of the program -- as Linux does.
 */
static int catches(struct task *t, int sig)
{
    sighandler_t h = t->sigact[sig].sa_handler;

    return h != SIG_DFL && h != SIG_IGN && !(t->sig_blocked & SIGMASK(sig));
}

/*
 * Turn the exception frame into a four-word format $0 frame at the
 * same PC, so that the signal machinery -- which redirects a frame by
 * changing its PC, and so accepts only frames an rte simply returns
 * through -- can send the program to its handler. The new frame goes at
 * the END of the old one; the number of bytes before it is what
 * _exc_common has to drop, moving the saved registers up to meet it
 * (Linux calls this the stack adjustment).
 */
static int make_format0(u16 *frame, unsigned fmt, unsigned vec)
{
    int size = fmt == 7 ? 60 : fmt == 2 || fmt == 3 ? 12 : 8;
    u16 sr = frame[0], pchi = frame[1], pclo = frame[2];
    u16 *nf = frame + (size - 8) / 2;

    nf[0] = sr;
    nf[1] = pchi;
    nf[2] = pclo;
    nf[3] = (u16)(vec << 2);            /* format 0 */
    return size - 8;
}

int exception_handler(const u32 *regs, u16 *frame)
{
    unsigned vec = (unsigned)((frame[3] & 0x0fff) >> 2);
    unsigned fmt = (unsigned)(frame[3] >> 12);
    int i;

    /*
     * A fault in a program kills the program, not the machine -- or,
     * if the program asked, becomes a signal it handles.
     *
     * DEMAND PAGING. An access fault (format $7) whose page is lazy, in
     * the swap file, or shared copy-on-write: vm_fault() makes it what
     * the access needs, and the pending write-backs are completed
     * (wb040.c) -- which on a real 68040 is what a store to a new page
     * needs, its write having been left in WB2 with the instruction
     * already past. Then the rte resumes: a read re-runs its
     * instruction, a store carries on after it.
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
        !wb040_push_fault(frame)) {
        static u32 last_addr, last_pc, nochange;
        u32 addr = fault_address(frame);
        int write = !(fault_ssw(frame) & SSW_RW);
        int r = vm_fault(current->as, addr, write);

        if (r == VM_FAULT_NOCHANGE) {
            if (addr == last_addr && frame_pc(frame) == last_pc) {
                nochange++;
            } else {
                nochange = 1;
            }
            last_addr = addr;
            last_pc = frame_pc(frame);
            if (nochange < 3) {
                r = 0;
            } else {
                nochange = 0;
            }
        } else if (r == 0) {
            nochange = 0;
        }
        if (r == 0) {
            struct fault_wb undone;

            memset(&undone, 0, sizeof(undone));
            if (wb040_complete(frame, &undone) == 0) {
                return 0;
            }
            /* A write-back whose own address is bad: that is the fault
             * the program is told of. */
            addr = undone.w[0].addr;
            r = -EFAULT;
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
        struct task *t = current;
        int code;
        int sig = fault_signal(vec, &code);
        u32 addr = frame_pc(frame);

        if (vec == 2 && fmt == 7) {
            addr = fault_address(frame);
            if (!(fault_ssw(frame) & SSW_ATC)) {
                sig = SIGBUS;                   /* a physical bus error */
                code = BUS_ADRERR;
            } else if (vm_translate(t->as, addr, 0)) {
                /* Resident -- readable, even -- and still refused: a
                 * write to a read-only page. (A PROT_NONE mapping is not
                 * resident and reads as MAPERR here, where Linux says
                 * ACCERR: this kernel keeps no record of mappings apart
                 * from their pages.) */
                code = SEGV_ACCERR;
            }
        } else if (vec == 3 && fmt == 2) {
            addr = ((u32)frame[4] << 16) | frame[5];
        }

        /*
         * A HANDLER: the program gets the signal, now -- _exc_common
         * returns through task_ret_to_user, which delivers it -- with
         * the fault's address and code for an SA_SIGINFO handler, and
         * any writes the instruction left undone carried in the frame
         * for sigreturn to finish. Resuming re-runs the faulting
         * instruction, or carries on after one that had completed, as
         * the frame's own PC says.
         */
        /* Or a TRACED program, whatever it catches: the tracer is told
         * of the signal first, at a signal-delivery stop, and a
         * debugger's breakpoints and steps are SIGTRAPs (ptrace.c). */
        if ((catches(t, sig) || t->tracer) && !(vec == 2 && fmt == 7 &&
                                 wb040_push_fault(frame))) {
            memset(&t->pending_wb, 0, sizeof(t->pending_wb));
            if (vec == 2 && fmt == 7) {
                wb040_complete(frame, &t->pending_wb);
            }
            t->fault_sig = sig;
            t->fault_code = code;
            t->fault_addr = addr;
            signal_send(t, sig);
            return make_format0(frame, fmt, vec);
        }

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
         * THE REGISTERS, because a fault address on its own says where
         * the program died and nothing about why. Which register held
         * the bad pointer is usually the whole answer -- and a program
         * that dies in somebody else's library, with no debugger on this
         * machine, leaves nothing else to go on.
         */
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
        /*
         * Its own fault ends its own task and nothing else, with the
         * status a shell reports for that signal: 128 plus its number
         * (139 for SIGSEGV). The kernel is intact -- the exception came
         * from user mode, so its stack is its own.
         */
        t->signalled = sig;
        t->core_dumped = core_dump(t, sig, regs, frame[0], frame_pc(frame),
                                   frame[3]);
        task_exit(128 + sig);
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

/*
 * A KERNEL STACK RAN OUT: a function's prologue found the stack pointer
 * below the limit in a5 (task.c, KSTACK_RED). Reached from start.s on a
 * stack of its own, with the registers as they were at the trap.
 *
 * What it says is what finding the culprit needs: which task, where
 * (the trapping prologue's address is in the format-2 frame), how deep,
 * and the return addresses still on the stack, nearest first -- words
 * just after a jsr or bsr in the kernel's text, which is all a stack
 * without frame pointers can offer. Some are stale, left by calls that
 * have since returned (a deep call of earlier leaves its words below
 * the frames that are live now); the live path is among them, in order.
 * `m68k-elf-addr2line -f -e kernel/kernel.elf` on them names it.
 */
/* Could `a` be a return address: inside the kernel's text, and just
 * after a jsr or a bsr? Stack words that merely fall in the text's range
 * -- a counter, a small constant -- are not, and are left out. */
static int looks_like_return(u32 a)
{
    extern char _start[], _etext_marker[];
    const u16 *w;

    if (a < (u32)_start + 6 || a >= (u32)_etext_marker || (a & 1)) {
        return 0;
    }
    w = (const u16 *)a;
    return (w[-1] & 0xfff8) == 0x4e90 ||       /* jsr (An)          */
           (w[-1] & 0xff00) == 0x6100 ||       /* bsr.s             */
           w[-2] == 0x4eba || w[-2] == 0x6100 || /* jsr/bsr.w d16(pc) */
           (w[-2] & 0xfff8) == 0x4ea8 ||       /* jsr d16(An)       */
           w[-3] == 0x4eb9 || w[-3] == 0x61ff;  /* jsr abs.l, bsr.l  */
}

void kstack_overflow(struct pt_regs *r)
{
    u32 lo, hi, sp = (u32)r + sizeof(*r) + 4;  /* + the format-2 address */
    u32 *p;
    int n = 0;

    __asm__ volatile ("move.w #0x2700,%sr");
    task_kstack_bounds(&lo, &hi);
    kputs("\n*** kernel stack overflow: ");
    kputs(current ? current->name : "?");
    kputs(" (pid ");
    kputdec(current ? (u32)current->pid : 0);
    kputs(")\n    in the function at ");
    kputhex32(*(u32 *)((u32)r + sizeof(*r)));   /* the TRAPcc's address */
    kputs(", sp ");
    kputhex32(sp);
    kputs(", ");
    kputdec(hi - sp);
    kputs(" of ");
    kputdec(hi - lo);
    kputs(" bytes used\n    return addresses, nearest first"
          " (a run of one is written once, with its count):");
    {
        u32 last = 0, reps = 0;

        for (p = (u32 *)sp; (u32)p <= hi && n < 24; p++) {
            u32 v = (u32)p < hi ? *p : 0;

            if ((u32)p < hi && !looks_like_return(v)) {
                continue;
            }
            if (v == last && v) {
                reps++;
                continue;
            }
            if (last) {
                kputs(n % 4 == 0 ? "\n      " : "  ");
                kputhex32(last);
                if (reps > 1) {
                    kputc('x');
                    kputdec(reps);
                }
                n++;
            }
            last = v;
            reps = 1;
        }
    }
    kputc('\n');
    panic("kernel stack overflow");
}

void panic(const char *msg)
{
    struct timeval tv;

    __asm__ volatile ("move.w #0x2700,%sr");
    kputs("\n*** panic: ");
    kputs(msg);
    kputs("\n");
    /* Kept in the NVRAM for the next boot to report (klog.c). */
    clock_get(&tv);
    klog_panic_save((u32)tv.tv_sec);
    kputs("*** halted.\n");
    halt();
}

void trap_init(void)
{
    extern void _trap0_entry(void), _trapcc_entry(void);
    u32 *vectors = (u32 *)_vectors;

    vectors[VEC_TRAP0] = (u32)_trap0_entry;
    vectors[VEC_TRAPCC] = (u32)_trapcc_entry;
    fpsp_install(vectors);
}
