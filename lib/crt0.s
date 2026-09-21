| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| crt0.s - what a program starts in.
|
| The kernel calls here with argc and argv on the stack it handed over.
| It arrives by `jsr`, so there is a return address below them -- which
| is never used, because a program leaves through exit() rather than by
| returning to its loader:
|
|       0(%sp)  return address, into the kernel
|       4(%sp)  argc
|       8(%sp)  argv
|      12(%sp)  envp
|
| .bss is not cleared here. The kernel does it, because it is the loader
| that knows which part of a segment the file supplied and which part it
| did not -- p_memsz beyond p_filesz. That is where Unix has always put
| it, and doing it twice would only hide a loader that had stopped.
|
| main() returning and main() calling exit() end the same way, which is
| what C requires.

        .section .text.start
        .globl  _start
        .type   _start,@function
_start:
        move.l  12(%sp),%a0             | envp
        move.l  %a0,environ             | for getenv(), before main runs
        move.l  8(%sp),%d0              | argv
        move.l  4(%sp),%d1              | argc
        move.l  %a0,-(%sp)
        move.l  %d0,-(%sp)
        move.l  %d1,-(%sp)
        jsr     main
        lea     12(%sp),%sp

        move.l  %d0,-(%sp)              | main's return value
        jsr     exit                    | does not come back
1:      bra.s   1b                      | ... but if it ever did

| Where getenv() looks. Set before main so that a program may call it
| from a constructor-like path as well as from main's arguments.
|
| Where every signal handler returns to. The kernel put the address here
| as the handler's return address (sigaction's sa_restorer, which the
| library always supplies), and the handler's rts has popped it, so the
| stack now holds the signal number and above it the saved context.
| sigreturn finds that context from the stack pointer and puts back
| everything the handler interrupted; it does not return here.
|
| It lives in the library rather than being written onto the stack by
| the kernel, as Linux does with sa_restorer. The 68040 could not have
| refused to run code on the stack -- it has no execute permission bit
| -- but code written there needs the caches pushed on real hardware,
| and a trampoline in the source can be read.
|
        .text
        .globl  __sigreturn_trampoline
        .type   __sigreturn_trampoline,@function
__sigreturn_trampoline:
        moveq   #119,%d0                | __NR_sigreturn
        trap    #0
        bra.s   __sigreturn_trampoline  | not reached

        .bss
        .align  4
        .globl  environ
environ:
        .space  4
