| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| execasm.s - switch to a program's stack, call it, and come back.
|
|   int exec_call(u32 entry, u32 stack_top, int argc, char **argv);
|
| Returns whatever the program's entry point returned, or whatever it
| passed to exit().
|
| Two things are going on here.
|
| The first is an ordinary stack switch: the program gets its own stack
| rather than growing down through the kernel's, so a program that
| recurses too far runs into empty space instead of into the kernel.
|
| The second is the unwind. A program that calls exit() is several frames
| deep inside itself and inside a trap handler, and none of that can be
| returned through normally. exec_unwind() throws all of it away: it
| restores the stack pointer saved on the way in and returns from
| exec_call as though the program had simply returned. That is a longjmp
| in everything but name.
|
| Note where the trap frame lands. The program runs in supervisor mode --
| there is no user mode yet -- so its stack IS the supervisor stack, and a
| `trap #0` from the program pushes its exception frame there. Abandoning
| that frame is safe precisely because the whole stack is being discarded.
|
| One program at a time. exec.c refuses a nested spawn, because there is
| one saved context here and a second would overwrite it.

        .text

        .globl  exec_call
        .type   exec_call,@function
exec_call:
        movem.l %d2-%d7/%a2-%a6,-(%sp)  | 11 registers, 44 bytes
        move.l  %sp,exec_ksp            | the point to come back to

        move.l  44+4(%sp),%a0           | entry
        move.l  44+8(%sp),%a1           | stack_top
        move.l  44+12(%sp),%d1          | argc
        move.l  44+16(%sp),%d2          | argv

        movea.l %a1,%sp                 | the program's stack from here on
        move.l  %d2,-(%sp)              | argv
        move.l  %d1,-(%sp)              | argc
        jsr     (%a0)                   | into the program
        | d0 holds its return value; fall through.

        .globl  exec_unwind
        .type   exec_unwind,@function
exec_unwind:
        movea.l exec_ksp,%sp
        movem.l (%sp)+,%d2-%d7/%a2-%a6
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
| with an exception frame on the program's stack. Neither is a problem:
| the frame is on the stack being discarded, and nothing is returning
| through it.
|
| The interrupt mask is. Leaving through the side door means no RTE ever
| restores it, and the shell would come back with every interrupt masked
| -- a machine that runs, prints its prompt, and then never ticks again.
|
| The stack is switched BEFORE the mask is lowered. The other order leaves
| a window in which an interrupt could arrive, be handled on the stack
| being abandoned, and re-enter this code.
|
        .globl  exec_abort
        .type   exec_abort,@function
exec_abort:
        move.l  4(%sp),%d0              | status, before the stack goes
        movea.l exec_ksp,%sp
        move.w  #0x2000,%sr             | supervisor, IPL 0
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

|
| void exec_stop(u32 *saved_sp) - ctrl-Z. Returns when fg resumes it.
| int  exec_resume(u32 saved_sp)  - fg. Returns the program's exit status.
|
| These two are one operation written twice, and between them they are a
| context switch -- the primitive the scheduler will be built out of.
| Each saves where it is, in the frame layout exec_call already uses, and
| goes to where the other left off.
|
| What makes it this small is that only the callee-saved registers need
| keeping. A stop only ever happens at a system call boundary, which is a
| C call boundary, where the caller has already spilled anything else it
| cared about. Stopping at an arbitrary instruction would mean saving
| every register and the program counter from the exception frame, which
| is a different and larger thing.
|
        .globl  exec_stop
        .type   exec_stop,@function
exec_stop:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        move.l  48(%sp),%a0             | saved_sp, past the 44 just pushed
        move.l  %sp,(%a0)               | where to come back to
        movea.l exec_ksp,%sp            | and away to the shell
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

        .globl  exec_resume
        .type   exec_resume,@function
exec_resume:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        move.l  %sp,exec_ksp            | the shell's way home, replaced
        movea.l 48(%sp),%sp             | into the stopped program
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| Where exec_call, exec_stop and exec_resume all come back to: the
| context of whoever is waiting for the program. exec_resume replaces it,
| which is what makes a program that was resumed exit to its resumer.
        .bss
        .align  4
exec_ksp:
        .space  4
