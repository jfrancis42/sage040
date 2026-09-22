| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| crt0-dyn.s - where a program linked against libc.so starts.
|
| ld.so has run by now: libc.so is loaded, relocated and initialised,
| and the stack is exactly as the kernel built it (crt0.s describes it).
| What is left is the program's OWN constructors and destructors, and
| they cannot be left to libc: __libc_init_array in libc.so walks
| libc.so's arrays, not this program's -- so here they are walked
| directly, between the symbols the linker brackets them with.
|
| Destructors run at exit() by way of atexit(), which libc runs before
| its own destructors -- and those include the one that flushes stdout,
| so output a destructor prints is not lost.

        .section .text.start
        .globl  _start
        .type   _start,@function
_start:
        move.l  12(%sp),environ         | a COPY relocation made this ours
        move.l  8(%sp),%a0              | argv[0], for program_invocation_name
        move.l  (%a0),-(%sp)            |   and getprogname (posix-more.c)
        jsr     __sage040_progname
        addq.l  #4,%sp
        lea     __preinit_array_start,%a2
        lea     __preinit_array_end,%a3
        bsr.s   walk_up
        lea     __init_array_start,%a2
        lea     __init_array_end,%a3
        bsr.s   walk_up
        pea     fini
        jsr     atexit
        addq.l  #4,%sp
        move.l  12(%sp),-(%sp)          | envp
        move.l  12(%sp),-(%sp)          | argv (the stack moved by 4)
        move.l  12(%sp),-(%sp)          | argc (and by 4 again)
        jsr     main
        lea     12(%sp),%sp
        move.l  %d0,-(%sp)
        jsr     exit                    | does not return
1:      bra.s   1b

| Call each function pointer in [a2, a3), in order. a2 and a3 are
| callee-saved, so the functions called leave them alone.
walk_up:
        cmp.l   %a3,%a2
        beq.s   2f
        move.l  (%a2)+,%a0
        jsr     (%a0)
        bra.s   walk_up
2:      rts
        .size   _start, . - _start

| The program's destructors, last first. Called from C, so it saves
| what C expects kept.
        .type   fini,@function
fini:
        movem.l %a2-%a3,-(%sp)
        lea     __fini_array_start,%a2
        lea     __fini_array_end,%a3
3:      cmp.l   %a2,%a3
        beq.s   4f
        move.l  -(%a3),%a0
        jsr     (%a0)
        bra.s   3b
4:      movem.l (%sp)+,%a2-%a3
        rts
        .size   fini, . - fini
