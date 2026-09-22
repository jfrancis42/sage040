| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| crt0.s - where a program linked with picolibc starts.
|
| The kernel enters exactly as it does for lib/crt0.s -- by jsr, so
| above the return address:
|
|       4(%sp)  argc
|       8(%sp)  argv
|      12(%sp)  envp
|
| and .bss is already clear, because the loader clears it. What this
| adds for a real C library is its environ, and its constructors:
| __libc_init_array runs .preinit_array and .init_array, which is where
| a static initialiser with side effects ends up, and exit() runs the
| destructors and flushes stdio on the way out.
|
| No TLS to set up: picolibc is built with thread-local storage off
| (libc/build.sh), because a process here has one thread.

        .section .text.start
        .globl  _start
        .type   _start,@function
_start:
        move.l  12(%sp),environ         | picolibc's own, from stdlib
        jsr     __libc_init_array
        move.l  12(%sp),-(%sp)          | envp
        move.l  12(%sp),-(%sp)          | argv (the stack moved by 4)
        move.l  12(%sp),-(%sp)          | argc (and by 4 again)
        jsr     main
        lea     12(%sp),%sp
        move.l  %d0,-(%sp)
        jsr     exit                    | does not return
1:      bra.s   1b
        .size   _start, . - _start
