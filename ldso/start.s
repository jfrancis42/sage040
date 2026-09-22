| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| start.s - where a dynamically linked program begins: in ld.so.
|
| The kernel loaded the program and this interpreter, built the stack
| exactly as it does for a static program --
|
|        (%sp)  a return address of zero, which nothing returns to
|       4(%sp)  argc
|       8(%sp)  argv
|      12(%sp)  envp, whose terminating NULL is followed by the auxv
|
| -- and started HERE instead of at the program's entry. dl_main loads
| and links the libraries and hands back the program's entry point,
| and the program is entered with the stack exactly as the kernel left
| it, so its crt0 cannot tell an interpreter ran first.

        .section .text.start
        .globl  _dl_start
        .type   _dl_start,@function
_dl_start:
        move.l  %sp,-(%sp)
        jsr     dl_main
        addq.l  #4,%sp
        move.l  %d0,%a0
        jmp     (%a0)
        .size   _dl_start, . - _dl_start

| long dl_syscall(long nr, a1, a2, a3, a4, a5, a6): Linux/m68k's
| convention -- d0 the number, d1-d5 then a0, trap #0, result in d0.
        .text
        .globl  dl_syscall
        .type   dl_syscall,@function
dl_syscall:
        movem.l %d2-%d5,-(%sp)
        move.l  20(%sp),%d0
        move.l  24(%sp),%d1
        move.l  28(%sp),%d2
        move.l  32(%sp),%d3
        move.l  36(%sp),%d4
        move.l  40(%sp),%d5
        move.l  44(%sp),%a0
        trap    #0
        movem.l (%sp)+,%d2-%d5
        rts
        .size   dl_syscall, . - dl_syscall
