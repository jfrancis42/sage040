| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| fpspglue.s - this kernel's side of Motorola's M68040 FPSP (kernel/fpsp/).
|
| The package is the handler for the floating-point instructions and
| operand types the MC68040 has no silicon for. It expects the host to
| supply twelve symbols -- the part of the package Motorola shipped as
| skeleton.sa, for each operating system to replace -- and those are
| here:
|
|   fpsp_done       the exception is dealt with: resume the program
|   real_xxx        it is real and must be reported: a signal
|   mem_read/write  copy up to 12 bytes from or to the program
|   fpsp_fmt_error  an FPU state frame the package does not know
|   real_trace      the program was being traced
|
| Each real_xxx first does what Motorola's skeleton.sa does -- clear the
| E1 or E3 bit in the FPU's state frame so that the exception is not
| taken again the moment the FPU is used -- and those sequences are
| Motorola's, from skeleton.sa 3.2 (see kernel/fpsp/README for the
| licence they are under). What follows them, and everything else here,
| is this kernel's.
|
| Entered straight from the exception vectors (fpsp_install, fpsp.c),
| with the CPU's exception frame on the supervisor stack and every
| register the program's. Nothing here or in the package may assume a
| register survives a call into C except d2-d7/a2-a6.

#include "fpsp/fpsp.h"

        .text

| Every way out that returns to the program goes through here: through
| task_ret_to_user, as a system call does, so that a signal raised
| meanwhile is delivered and a task that should give up the CPU does.
| From supervisor mode it simply returns: the kernel does no floating
| point, so that is only ever the package's own bookkeeping.
        .macro  RETURN
        btst    #5,(%sp)                | S bit of the saved SR
        bne.s   8f
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        move.l  kstack_limit,%a5        | the stack limit: see task.c
        move.l  %sp,-(%sp)              | -> struct pt_regs
        jsr     task_ret_to_user
        addq.l  #4,%sp
        movem.l (%sp)+,%d0-%d7/%a0-%a6
8:      rte
        .endm

| Report: post `sig` to the program (fpsp_report, fpsp.c), then leave
| as above -- which is where the signal is delivered.
        .macro  REPORT sig
        movem.l %d0-%d7/%a0-%a6,-(%sp)
        move.l  kstack_limit,%a5        | the stack limit: see task.c
        pea     \sig
        pea     4(%sp)                  | -> struct pt_regs
        jsr     fpsp_report
        addq.l  #8,%sp
        move.l  %sp,-(%sp)
        jsr     task_ret_to_user
        addq.l  #4,%sp
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        rte
        .endm

        .set    SIGILL, 4
        .set    SIGFPE, 8

| ------------------------------------------------------------------
| Vector stubs. Vector 11 is shared by the unimplemented floating-point
| instruction (a format $2 frame, vector offset $2C) and the F-line
| illegal instruction (format $0); the package tells them apart itself,
| and this only counts. The frame's format/vector word is at 6(%sp).
| ------------------------------------------------------------------
        .globl  fpsp_vec_fline
fpsp_vec_fline:
        cmp.w   #0x202c,6(%sp)
        bne.s   1f
        addq.l  #1,fpsp_stats          | unimp
1:      jmp     fpsp_fline

        .globl  fpsp_vec_unsupp
fpsp_vec_unsupp:
        addq.l  #1,fpsp_stats+4        | unsupp
        jmp     fpsp_unsupp

| ------------------------------------------------------------------
| The way back.
| ------------------------------------------------------------------
        .globl  fpsp_done
fpsp_done:
        RETURN

| Tracing a program is not something this kernel does (there is no
| ptrace), so a trace the package passes on has nowhere to go.
        .globl  real_trace
real_trace:
        RETURN

| ------------------------------------------------------------------
| The exceptions the package decides are real. From skeleton.sa: each
| clears its own pending bit in the FPU state frame (E1 detected by the
| conversion unit, E3 by write-back), then reports.
| ------------------------------------------------------------------

| Divide by zero is always real, so it has no fpsp_dz; the vector
| points here.
        .globl  real_dz
real_dz:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
        bclr.b  #E1,E_BYTE(%a6)
        frestore (%sp)+
        unlk    %a6
        addq.l  #1,fpsp_stats+12       | reported
        REPORT  SIGFPE

| Inexact is always real too. The code before real_inex is skeleton.sa's
| fix for MC68040 bug #1232 on version $40 parts: an E1 SNAN, OVFL or
| UNFL taken after a context switch arrives as INEX instead, and has to
| be sent where it belongs.
        .globl  inex
inex:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
        cmp.b   #VER_40,(%sp)
        bne.s   not_fmt40
        fmove.l %fpsr,-(%sp)
        btst.b  #E1,E_BYTE(%a6)
        beq.s   not_b1232
        btst.b  #snan_bit,2(%sp)
        beq.s   inex_ckofl
        addq.l  #4,%sp
        frestore (%sp)+
        unlk    %a6
        jmp     fpsp_snan
inex_ckofl:
        btst.b  #ovfl_bit,2(%sp)
        beq.s   inex_ckufl
        addq.l  #4,%sp
        frestore (%sp)+
        unlk    %a6
        jmp     fpsp_ovfl
inex_ckufl:
        btst.b  #unfl_bit,2(%sp)
        beq.s   not_b1232
        addq.l  #4,%sp
        frestore (%sp)+
        unlk    %a6
        jmp     fpsp_unfl
not_b1232:
        addq.l  #4,%sp
        frestore (%sp)+
        unlk    %a6

        .globl  real_inex
real_inex:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
not_fmt40:
        bclr.b  #E3,E_BYTE(%a6)
        beq.s   inex_cke1
        | An E3: clear the destination's dirty bit, then skeleton.sa's
        | check for MC68040 bug #1238.
        movem.l %d0/%d1,USER_DA(%a6)
        bfextu  CMDREG1B(%a6){#6:#3},%d0
        bclr.b  %d0,FPR_DIRTY_BITS(%a6)
        bsr.l   b1238_fix
        movem.l USER_DA(%a6),%d0/%d1
        bra.s   inex_done
inex_cke1:
        bclr.b  #E1,E_BYTE(%a6)
inex_done:
        frestore (%sp)+
        unlk    %a6
        addq.l  #1,fpsp_stats+12
        REPORT  SIGFPE

        .globl  real_ovfl
real_ovfl:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
        bclr.b  #E3,E_BYTE(%a6)
        bne.s   1f
        bclr.b  #E1,E_BYTE(%a6)
1:      frestore (%sp)+
        unlk    %a6
        addq.l  #1,fpsp_stats+12
        REPORT  SIGFPE

        .globl  real_unfl
real_unfl:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
        bclr.b  #E3,E_BYTE(%a6)
        bne.s   1f
        bclr.b  #E1,E_BYTE(%a6)
1:      frestore (%sp)+
        unlk    %a6
        addq.l  #1,fpsp_stats+12
        REPORT  SIGFPE

        .globl  real_snan
real_snan:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
        bclr.b  #E1,E_BYTE(%a6)         | always an E1
        frestore (%sp)+
        unlk    %a6
        addq.l  #1,fpsp_stats+12
        REPORT  SIGFPE

        .globl  real_operr
real_operr:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
        bclr.b  #E1,E_BYTE(%a6)         | always an E1
        frestore (%sp)+
        unlk    %a6
        addq.l  #1,fpsp_stats+12
        REPORT  SIGFPE

| BSUN: skeleton.sa also clears the NaN condition code, or the branch
| that raised it would raise it again on return.
        .globl  real_bsun
real_bsun:
        link    %a6,#-LOCAL_SIZE
        fsave   -(%sp)
        bclr.b  #E1,E_BYTE(%a6)
        fmove.l %fpsr,-(%sp)
        bclr.b  #nan_bit,(%sp)
        fmove.l (%sp)+,%fpsr
        frestore (%sp)+
        unlk    %a6
        addq.l  #1,fpsp_stats+12
        REPORT  SIGFPE

| An F-line instruction that is not floating point at all.
        .globl  real_fline
real_fline:
        addq.l  #1,fpsp_stats+8        | fline
        REPORT  SIGILL

| An FPU state frame the package does not recognise: a revision of the
| chip it was not written for. Nothing can be resumed from the middle of
| the package, so the program ends, saying why (fpsp.c).
        .globl  fpsp_fmt_error
fpsp_fmt_error:
        addq.l  #1,fpsp_stats+16       | bad_frame
        move.l  kstack_limit,%a5
        jsr     fpsp_bad_frame
        | does not return

| ------------------------------------------------------------------
| mem_read / mem_write: a0 source, a1 destination, d0 a count of at
| most 12, one end of it the supervisor stack. From supervisor mode a
| byte copy (the package's own convention); from a program, through
| copy_from_user/copy_to_user, which translate in software and may
| sleep on a page fault -- the FPU's state is safe in the task's save
| area if they do. A copy that fails ends the program (fpsp.c): the
| package cannot be told an operand was unreadable.
| ------------------------------------------------------------------
        .globl  mem_read
mem_read:
        btst.b  #5,EXC_SR(%a6)
        beq.s   1f
2:      move.b  (%a0)+,(%a1)+
        subq.l  #1,%d0
        bne.s   2b
        rts
1:      move.l  %d1,-(%sp)
        move.l  %a5,-(%sp)              | the package's, put back after
        move.l  kstack_limit,%a5        | C runs under the limit: task.c
        move.l  %d0,-(%sp)              | len
        move.l  %a0,-(%sp)              | the program's address
        move.l  %a1,-(%sp)              | where it goes
        jsr     copy_from_user
        lea     12(%sp),%sp
        move.l  (%sp)+,%a5
        tst.l   %d0
        bne.s   3f
        move.l  (%sp)+,%d1
        rts
3:      move.l  kstack_limit,%a5
        jsr     fpsp_bad_copy
        | does not return

        .globl  mem_write
mem_write:
        btst.b  #5,EXC_SR(%a6)
        beq.s   1f
2:      move.b  (%a0)+,(%a1)+
        subq.l  #1,%d0
        bne.s   2b
        rts
1:      move.l  %d1,-(%sp)
        move.l  %a5,-(%sp)              | the package's, put back after
        move.l  kstack_limit,%a5        | C runs under the limit: task.c
        move.l  %d0,-(%sp)              | len
        move.l  %a0,-(%sp)              | from the stack
        move.l  %a1,-(%sp)              | the program's address
        jsr     copy_to_user
        lea     12(%sp),%sp
        move.l  (%sp)+,%a5
        tst.l   %d0
        bne.s   3f
        move.l  (%sp)+,%d1
        rts
3:      move.l  kstack_limit,%a5
        jsr     fpsp_bad_copy
        | does not return
