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
        move.l  8(%sp),%d0              | argv
        move.l  4(%sp),%d1              | argc
        move.l  %d0,-(%sp)
        move.l  %d1,-(%sp)
        jsr     main
        addq.l  #8,%sp

        move.l  %d0,-(%sp)              | main's return value
        jsr     exit                    | does not come back
1:      bra.s   1b                      | ... but if it ever did
