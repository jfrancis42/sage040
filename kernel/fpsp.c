/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fpsp.c - the kernel's C side of Motorola's M68040 FPSP.
 *
 * The MC68040 has part of the MC68881/MC68882 instruction set in
 * silicon; the rest -- FSIN, FETOX, FLOGN and the other transcendentals,
 * FINT, FMOD, FREM, FSCALE, FMOVECR (MC68040 User's Manual, Table 9-10)
 * -- raises the unimplemented floating-point instruction exception, and
 * operands it cannot take (denormal, unnormal, packed decimal) raise
 * unsupported data type. Motorola's FPSP, in kernel/fpsp/, is the
 * handler for both, and for the arithmetic exceptions besides; it is
 * what makes a program that says `fsin` work on a real 68040 at all.
 * kernel/fpspglue.s connects it to this kernel; this is what that needs
 * from C.
 *
 * Under QEMU the instructions run directly, and the package is never
 * reached -- unless the CPU is started as `-cpu m68040,fpsp-trap=on`,
 * which makes it trap as the silicon does. kernel/fpsptest.sh does
 * that, and uses the counters here to prove the package, and not the
 * CPU, computed the answers.
 */
#include "kernel.h"
#include "console.h"
#include "task.h"
#include "signal.h"
#include "ptregs.h"
#include "uapi.h"
#include "string.h"

/* Read and incremented by fpspglue.s as well as here: the offsets of
 * its members are hard-coded there, in struct fpspstats order. */
struct fpspstats fpsp_stats;

void fpsp_counts(struct fpspstats *out)
{
    *out = fpsp_stats;
}

/*
 * The package decided the exception is real: post the signal, and
 * fpspglue.s delivers it on the way out. The kernel does no floating
 * point, so one from supervisor mode is a kernel bug.
 */
void fpsp_report(struct pt_regs *regs, int sig)
{
    struct task *t = current;
    unsigned vec = (regs->format & 0x0fff) >> 2;
    int code;

    if (!pt_user_mode(regs) || !t || !t->as) {
        kputs("\n*** floating-point exception in the kernel, pc=0x");
        kputhex32(regs->pc);
        kputln("");
        panic("FPSP: exception from supervisor mode");
    }
    /* Linux/m68k's si_code for each, and the PC as si_addr. */
    switch (vec) {
    case 49: code = FPE_FLTRES; break;          /* inexact          */
    case 50: code = FPE_FLTDIV; break;          /* divide by zero   */
    case 51: code = FPE_FLTUND; break;          /* underflow        */
    case 53: code = FPE_FLTOVF; break;          /* overflow         */
    case 48:                                    /* BSUN             */
    case 52:                                    /* operand error    */
    case 54: code = FPE_FLTINV; break;          /* signalling NaN   */
    default: code = ILL_ILLOPC; break;          /* F-line: SIGILL   */
    }
    memset(&t->pending_wb, 0, sizeof(t->pending_wb));
    t->fault_sig = sig;
    t->fault_code = code;
    t->fault_addr = regs->pc;
    signal_send(t, sig);
}

static void fpsp_kill(const char *why, int sig)
{
    kputs("\nFPSP: ");
    kputln(why);
    if (!current || !current->as) {
        panic("FPSP: from a kernel task");
    }
    current->signalled = sig;
    task_exit(128 + sig);
}

/* mem_read or mem_write could not reach the program's memory. The
 * package has no way to be told, so the program ends -- as it would have
 * for touching that address itself. */
void fpsp_bad_copy(void)
{
    fpsp_kill("an operand the program cannot reach", SIGSEGV);
}

/* An FPU state frame of a version or size the package does not know:
 * a revision of the chip it was not written for. */
void fpsp_bad_frame(void)
{
    fpsp_kill("an FPU state frame of an unknown kind", SIGILL);
}

/*
 * The vectors the package handles. Vector 11 is F-line: the
 * unimplemented floating-point instruction and the F-line illegal
 * instruction share it, and the package tells them apart by the frame.
 * 48-55 are the floating-point exceptions, in the order the 68040
 * numbers them.
 */
void fpsp_install(u32 *vectors)
{
    extern void fpsp_vec_fline(void), fpsp_vec_unsupp(void);
    extern void fpsp_bsun(void), inex(void), real_dz(void);
    extern void fpsp_unfl(void), fpsp_operr(void), fpsp_ovfl(void);
    extern void fpsp_snan(void);

    vectors[11] = (u32)fpsp_vec_fline;
    vectors[48] = (u32)fpsp_bsun;       /* branch/set on unordered */
    vectors[49] = (u32)inex;            /* inexact result          */
    vectors[50] = (u32)real_dz;         /* divide by zero          */
    vectors[51] = (u32)fpsp_unfl;       /* underflow               */
    vectors[52] = (u32)fpsp_operr;      /* operand error           */
    vectors[53] = (u32)fpsp_ovfl;       /* overflow                */
    vectors[54] = (u32)fpsp_snan;       /* signalling NaN          */
    vectors[55] = (u32)fpsp_vec_unsupp; /* unsupported data type   */
}
