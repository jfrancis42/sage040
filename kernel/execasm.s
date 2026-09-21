| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| execasm.s - entering a program, leaving one, and swapping between.
|
| A program runs in USER MODE, in its own address space, on its own
| stack. None of those are things the kernel can simply call into, so
| everything here is about crossing that boundary and getting back.
|
| THE SAVED CONTEXT, used identically by all four routines below:
|
|       0(sp)   d2-d7/a2-a6     44 bytes, the callee-saved registers
|      44(sp)   USP             the user stack pointer
|      48(sp)   return address
|
| USP is in there because the 68040 keeps the user and supervisor stack
| pointers in separate registers, and there is exactly one of each. A
| program stopped by ctrl-Z has its stack pointer sitting in USP, and the
| next program to run overwrites it. Saving it alongside the registers is
| what makes `fg` land back where the program actually was.
|
| Only the callee-saved registers are kept, and that is a real
| limitation: it works because every switch happens at a C call boundary
| inside the kernel, where the caller has already spilled anything else
| it cared about. Stopping a program at an arbitrary instruction would
| mean saving every register and the PC out of the exception frame --
| which is a different and larger thing, and is what a scheduler will
| have to do.

        .text

| ------------------------------------------------------------------
| int exec_enter(u32 entry, u32 usp, u32 kstack_top)
|
| Go to user mode. Returns only through exec_unwind, when the program
| exits or is killed.
|
| THE PROGRAM GETS ITS OWN SUPERVISOR STACK, and that is not a detail.
| Every trap and every interrupt the program takes pushes its frame on
| whatever the supervisor stack is at the time; if that were the shell's
| stack, then the moment the program stopped and the shell carried on,
| the shell would grow down over the very frames the program has to
| return through. It would look like it worked -- fg would resume into a
| context that had been overwritten by whatever the shell did next.
| Switching stacks here is what makes a stopped program actually
| resumable, and it is the same reason every task in a real system has a
| kernel stack of its own.
|
| The 68040 has no instruction for "drop privilege". The way down is to
| build an exception frame that claims to have come from user mode and
| then return from it -- so this pushes a format 0 frame whose saved SR
| has the supervisor bit clear, and RTEs into it. The CPU restores that
| SR, notices S is now clear, and switches A7 from the supervisor stack
| to USP on the way out.
|
| SR is 0x0000: user mode, all interrupts enabled, trace off. A program
| cannot mask an interrupt, because the register that would let it do so
| is not writable from where it is standing.
| ------------------------------------------------------------------
        .globl  exec_enter
        .type   exec_enter,@function
exec_enter:
        move.l  %usp,%a0                | whatever was there before
        move.l  %a0,-(%sp)
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        move.l  %sp,exec_ksp            | the point to come back to

        move.l  52(%sp),%d0             | entry
        move.l  56(%sp),%a0             | usp
        move.l  60(%sp),%a1             | top of its supervisor stack
        move.l  %a0,%usp
        movea.l %a1,%sp                 | from here, traps land on its stack

        clr.w   -(%sp)                  | format 0, vector 0
        move.l  %d0,-(%sp)              | PC  = the program's entry point
        clr.w   -(%sp)                  | SR  = user mode, IPL 0
        rte                             | ... and away

| ------------------------------------------------------------------
| The way back.
|
| exec_unwind throws away whatever the program was doing -- its user
| stack, the trap frame it was in the middle of, all of it -- and returns
| from exec_enter as though it had simply returned. That is safe
| precisely because everything being discarded belongs to the program,
| and the program is over.
| ------------------------------------------------------------------
        .globl  exec_unwind
        .type   exec_unwind,@function
exec_unwind:
        movea.l exec_ksp,%sp
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        move.l  (%sp)+,%a0
        move.l  %a0,%usp
        rts

|
| void exec_longjmp(int status) - used by exit(). Never returns.
|
        .globl  exec_longjmp
        .type   exec_longjmp,@function
exec_longjmp:
        move.l  4(%sp),%d0
        bra     exec_unwind

|
| void exec_abort(int status) - the same unwind, but safe from an
| interrupt handler. Never returns.
|
| ctrl-C on a program that is making no system calls is noticed by the
| timer interrupt, so the unwind starts inside a handler running at IPL 6
| with an exception frame on the supervisor stack. Neither is a problem:
| nothing is returning through that frame, and the stack it sits on is
| about to be reset to exec_ksp anyway.
|
| The interrupt mask is. Leaving through the side door means no RTE ever
| restores it, and the shell would come back with every interrupt masked
| -- a machine that runs, prints its prompt, and then never ticks again.
|
| The stack is switched BEFORE the mask is lowered. The other order
| leaves a window in which an interrupt could arrive, be handled on the
| stack being abandoned, and re-enter this code.
|
        .globl  exec_abort
        .type   exec_abort,@function
exec_abort:
        move.l  4(%sp),%d0              | status, before the stack goes
        movea.l exec_ksp,%sp
        move.w  #0x2000,%sr             | supervisor, IPL 0
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        move.l  (%sp)+,%a0
        move.l  %a0,%usp
        rts

| ------------------------------------------------------------------
| void exec_stop(u32 *saved_sp)  - ctrl-Z. Returns when fg resumes it.
| int  exec_resume(u32 saved_sp) - fg. Returns the program's exit status.
|
| These two are one operation written twice, and between them they are a
| context switch -- the primitive the scheduler will be built out of.
| Each saves where it is in the layout described at the top of this file
| and goes to where the other left off.
| ------------------------------------------------------------------
        .globl  exec_stop
        .type   exec_stop,@function
exec_stop:
        move.l  %usp,%a0
        move.l  %a0,-(%sp)
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        move.l  52(%sp),%a0             | saved_sp, past the 48 just pushed
        move.l  %sp,(%a0)               | where to come back to
        movea.l exec_ksp,%sp            | and away to whoever was waiting
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        move.l  (%sp)+,%a0
        move.l  %a0,%usp
        rts

        .globl  exec_resume
        .type   exec_resume,@function
exec_resume:
        move.l  %usp,%a0
        move.l  %a0,-(%sp)
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        move.l  %sp,exec_ksp            | the resumer's way home, replaced
        movea.l 52(%sp),%sp             | into the stopped program
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        move.l  (%sp)+,%a0
        move.l  %a0,%usp
        rts

| Where exec_enter, exec_stop and exec_resume all come back to: the
| context of whoever is waiting for the program. exec_resume replaces it,
| which is what makes a program that was resumed exit to its resumer.
        .bss
        .align  4
exec_ksp:
        .space  4
