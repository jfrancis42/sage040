| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| taskasm.s - the context switch, and the way into a brand new task.
|
| There is one switch here and everything uses it. A task blocking, a
| task being preempted at the end of an interrupt, and a task exiting all
| arrive at switch_context() by way of schedule(); the difference between
| them is what schedule() decided, not how the switch is done.
|
| THE SAVED CONTEXT:
|
|       0(sp)   d2-d7/a2-a6     44 bytes, the callee-saved registers
|      44(sp)   USP             the user stack pointer
|      48(sp)   return address
|
| Only the callee-saved registers, because a switch always happens inside
| a C function call -- so the caller has already spilled anything else it
| cared about, and the C ABI says the rest are ours to destroy. The
| registers a PREEMPTED task was using are not in here at all: they were
| pushed by the interrupt stub, further up the same stack, and they come
| back when that stub's epilogue runs.
|
| USP is here because the 68040 keeps the user and supervisor stack
| pointers in separate registers and there is exactly one of each. Every
| task has its own user stack, so it has to travel with the context.

        .text

| ------------------------------------------------------------------
| void switch_context(u32 *save_sp, u32 new_sp)
|
| Save where we are into *save_sp, and continue from new_sp.
|
| It returns -- but to the OTHER task, the one whose stack pointer was
| passed in, at the point where IT called switch_context however long
| ago. The task that called this one comes back here later, when somebody
| switches to it.
| ------------------------------------------------------------------
        .globl  switch_context
        .type   switch_context,@function
switch_context:
        move.l  %usp,%a0
        move.l  %a0,-(%sp)
        movem.l %d2-%d7/%a2-%a6,-(%sp)

        move.l  52(%sp),%a0             | save_sp, past the 48 just pushed
        move.l  %sp,(%a0)               | where to come back to
        movea.l 56(%sp),%sp             | and away

        movem.l (%sp)+,%d2-%d7/%a2-%a6
        move.l  (%sp)+,%a0
        move.l  %a0,%usp
        rts

| ------------------------------------------------------------------
| void fpu_save(u32 *area) / void fpu_restore(const u32 *area)
|
| The FPU's state, in the order the 68040 requires: fsave first, which
| captures whatever the FPU was in the middle of and leaves it idle, and
| the programmer-visible registers only if the frame is not NULL -- a
| null frame means the FPU has never been used and holds nothing. On the
| way back the registers go in first and frestore last.
|
| Layout, matching struct task's fpu[]:
|       0       the state frame, up to 96 bytes
|      96       fp0-fp7, 12 bytes each
|     192       fpcr, fpsr, fpiar
| ------------------------------------------------------------------
        .globl  fpu_save
        .type   fpu_save,@function
fpu_save:
        move.l  4(%sp),%a0
        fsave   (%a0)
        tst.b   (%a0)
        beq.s   1f
        fmovem.x %fp0-%fp7,96(%a0)
        fmovem.l %fpcr/%fpsr/%fpiar,192(%a0)
1:      rts

        .globl  fpu_restore
        .type   fpu_restore,@function
fpu_restore:
        move.l  4(%sp),%a0
        tst.b   (%a0)
        beq.s   1f
        fmovem.x 96(%a0),%fp0-%fp7
        fmovem.l 192(%a0),%fpcr/%fpsr/%fpiar
1:      frestore (%a0)
        rts

| ------------------------------------------------------------------
| Where a new task begins.
|
| task_new() builds a kernel stack that looks exactly like one belonging
| to a task suspended in switch_context, with the return address pointing
| here. So the first time it is scheduled, switch_context's rts lands on
| this instruction with a full set of registers and an exception frame
| waiting above -- and the RTE takes it wherever the frame says, in
| whatever mode the frame's SR says.
|
| That is the same epilogue every interrupt stub ends with, which is the
| point: a task that has never run and a task returning from its ten
| thousandth interrupt leave the kernel by identical instructions.
| ------------------------------------------------------------------
        .globl  task_entry
        .type   task_entry,@function
task_entry:
        jsr     task_entry_hook         | bookkeeping, before it runs
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        rte
