| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| crt0-qemu.s - where a SuckOS program starts when it is run by
| qemu-m68k's LINUX-USER emulation, on a workstation, with no Sage040
| and no kernel of ours underneath it at all.
|
| WHY THIS WORKS, AND WHY IT IS WORTH HAVING. This system's ABI is
| Linux/m68k's on purpose: the system call numbers, the calling
| convention (d0 = number, d1-d5 = arguments, negated errno in d0) and
| the errnos. qemu-m68k emulates exactly that. So a statically linked
| program built for this machine is, as far as the emulator can tell,
| a Linux/m68k program -- and it runs.
|
| That is worth two things. It is the strongest evidence available
| that the ABI claim is true rather than approximately true: nothing
| in this tree is involved in the verdict. And it gives a way to run a
| program built for the machine WITHOUT booting the machine, which is
| seconds rather than a minute and is what CLISP's build will need --
| CLISP compiles a C program and then runs it to produce its Lisp
| image, and a cross build has to execute a target binary partway
| through (progress.md, task 48).
|
| Only static programs: /lib/ld.so is the Sage040's loader at the
| Sage040's paths.
|
| The difference from libc/crt0.s is the shape of the initial stack.
| This kernel enters a program with `jsr` and hands it three pointers:
| a return address, then argc, argv and envp as VALUES. Linux -- and
| so qemu's linux-user emulation -- starts at _start with
|
|       0(%sp)  argc
|       4(%sp)  argv[0]      <- the ARRAY is here, not a pointer to it
|       ...     argv[argc-1]
|               NULL
|               envp[0] ...
|
| so argv is an ADDRESS computed from the stack pointer, and envp is
| found by stepping over argc entries and the terminating null. Reading
| 4(%sp) as though it were `argv` gets argv[0] -- a char * -- and
| dereferencing that lands on the first four characters of the program
| name, which is exactly the fault seen: si_addr=0x2f746d70, "/tmp".
|
| d2/a2/a3 because they are callee-saved: the calls below would
| otherwise lose argc, argv and envp.
        .text
        .globl  _start
        .type   _start, @function
_start:
        move.l  (%sp),%d2               | argc
        lea     4(%sp),%a2              | argv
        move.l  %d2,%d0
        addq.l  #1,%d0                  | argc + 1, for the NULL
        lsl.l   #2,%d0                  | times sizeof(char *)
        lea     0(%a2,%d0.l),%a3        | envp
        move.l  %a3,environ

        move.l  (%a2),-(%sp)            | argv[0]
        jsr     __sage040_progname
        addq.l  #4,%sp
        jsr     __libc_init_array

        move.l  %a3,-(%sp)              | envp
        move.l  %a2,-(%sp)              | argv
        move.l  %d2,-(%sp)              | argc
        jsr     main
        lea     12(%sp),%sp
        move.l  %d0,-(%sp)
        jsr     exit
1:      bra.s   1b
        .size   _start, . - _start
