| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| start.s - Sage040 kernel entry.
|
| Two ways in, both landing on _start:
|
|   * the boot ROM reads KERNEL.ROM to address 0 and takes the initial
|     SSP and PC from the image's own first two longwords, which are
|     vectors 0 and 1 of the table below -- the 68000 reset convention;
|   * QEMU's -kernel loads the ELF and jumps to its entry point.
|
| The kernel runs in supervisor mode from here on and never leaves it.
| User programs will, and will re-enter through the TRAP #0 gate; the
| supervisor stack set up here is the stack their traps arrive on.

        .text
        .globl  _start
        .type   _start,@function
_start:
        move.w  #0x2700,%sr             | supervisor, all interrupts masked
        lea     _stack_top,%sp

        | The vector table is the first thing in the image, at address 0,
        | but point VBR at it explicitly rather than relying on that: the
        | 68040 comes out of reset with VBR = 0 and nothing else may.
        lea     _vectors,%a0
        movec   %a0,%vbr

        | Zero .bss
        lea     __bss_start,%a0
        lea     __bss_end,%a1
1:      cmpa.l  %a1,%a0
        bcc.s   2f
        clr.b   (%a0)+
        bra.s   1b
2:
        jsr     kmain
        | kmain() is not expected to return.
        .globl  halt
        .type   halt,@function
halt:
        stop    #0x2700
        bra.s   halt

|
| Common exception entry.
|
| Every vector except reset points here.  The 68040 pushes a format word
| containing the vector offset on every exception, so one handler can work
| out which exception it is rather than needing 255 separate stubs.
|
| Registers are saved so the C handler can report them, and the frame
| pointer is passed as its argument.
|
        .globl  _exc_common
        .type   _exc_common,@function
_exc_common:
        movem.l %d0-%d7/%a0-%a6,-(%sp)  | 15 registers, 60 bytes
        lea     60(%sp),%a0             | -> the exception frame
        move.l  %a0,-(%sp)
        move.l  %sp,%a0
        addq.l  #4,%a0                  | -> the saved registers
        move.l  %a0,-(%sp)
        jsr     exception_handler
        addq.l  #8,%sp
        movem.l (%sp)+,%d0-%d7/%a0-%a6
        rte

|
| TRAP #0 - the system call gate.
|
| The convention is Linux/m68k's, unchanged: d0 holds the call number,
| d1 through d5 the first five arguments and a0 the sixth, and d0 comes
| back holding the result or a negated errno.  d0 is deliberately not
| restored for that reason.  Only mmap2 takes six; a0 is Linux/m68k's
| sixth-argument register, as glibc's m68k sysdep.h uses it.
|
| Linux picked the obvious convention for this architecture and there is
| nothing to improve on, so a program written against one will work
| against the other as far as the calls themselves match.
|
        .globl  _trap0_entry
        .type   _trap0_entry,@function
| The saved SR is passed as an eighth argument, because what happens on
| the way out depends on where this call came from: a program returning
| to user mode may be signalled or preempted, and the kernel calling the
| gate on its own behalf may not. It is the first word of the exception
| frame, which sits above the 52 bytes of registers.
_trap0_entry:
        movem.l %d1-%d7/%a0-%a6,-(%sp)  | 13 registers, 52 bytes
        clr.l   -(%sp)
        move.w  56(%sp),2(%sp)          | the saved SR, zero extended
                                        | into the LOW half: a word
                                        | written at (%sp) would land in
                                        | the high half on a big-endian
                                        | machine and arrive multiplied
                                        | by 65536
        move.l  %a0,-(%sp)
        move.l  %d5,-(%sp)
        move.l  %d4,-(%sp)
        move.l  %d3,-(%sp)
        move.l  %d2,-(%sp)
        move.l  %d1,-(%sp)
        move.l  %d0,-(%sp)
        jsr     syscall_dispatch
        lea     32(%sp),%sp
        movem.l (%sp)+,%d1-%d7/%a0-%a6
        rte

|
| Vector table.  Entry 0 is the initial SSP and entry 1 the initial PC,
| which is what the boot ROM reads; the remaining 254 are filled in with
| _exc_common here and refined by trap_init() at runtime.
|
        .section .vectors,"a"
        .globl  _vectors
_vectors:
        .long   _stack_top              | 0: initial interrupt stack pointer
        .long   _start                  | 1: initial program counter
        .rept   254
        .long   _exc_common
        .endr
