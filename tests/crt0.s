| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
| crt0.s - Sage040 entry point.
|
| QEMU's sage040 machine loads a big-endian ELF32 and starts at the ELF
| entry point with SP already at the top of RAM, so all we must do is
| clear .bss, set up a minimal vector table, and call main().

        .text
        .globl  _start
        .type   _start,@function
_start:
        move.w  #0x2700,%sr             | supervisor, all interrupts masked
        lea     _stack_top,%sp

        | Point VBR at our vector table (68010+ feature; 68040 has it).
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
        jsr     main
        | main() returning is a normal end-of-test; fall into halt.
        .globl  halt
        .type   halt,@function
halt:
        stop    #0x2700
        bra.s   halt

|
| Default exception handler: print "EXC" plus the vector number so a
| stray trap is loud instead of silent, then halt.
|
        .globl  _exc_default
_exc_default:
        move.l  %d0,-(%sp)
        move.l  %a0,-(%sp)
        lea     0xff000000,%a0          | UART THR
        move.b  #'!',(%a0)
        move.b  #'E',(%a0)
        move.b  #'X',(%a0)
        move.b  #'C',(%a0)
        move.b  #'!',(%a0)
        move.b  #13,(%a0)
        move.b  #10,(%a0)
        move.l  (%sp)+,%a0
        move.l  (%sp)+,%d0
        stop    #0x2700

|
| Vector table: 256 entries.  Entry 0 is the initial SSP and entry 1 the
| initial PC (used on a real power-on reset); the rest point at the
| default handler unless a test overrides them at runtime.
|
        .section .vectors,"a"
        .globl  _vectors
_vectors:
        .long   _stack_top              | 0: initial SSP
        .long   _start                  | 1: initial PC
        .rept   254
        .long   _exc_default
        .endr
