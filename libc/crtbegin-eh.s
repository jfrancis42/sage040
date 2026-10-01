| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| crtbegin-eh.s - a program's unwind tables, handed to the unwinder.
|
| A C++ throw finds its way up the stack through each function's entry
| in .eh_frame. libgcc for m68k-elf -- this toolchain's -- finds that
| section only by being TOLD where it is, through __register_frame_info,
| which on a stock system crtbegin.o calls at start. Nothing here did,
| so every throw on the machine ended in libgcc's own assertion, a
| `trap #7` in uw_init_context_1 that the kernel reports as a TRAP
| instruction and the program dies of: gdb could not run at all.
|
| This is the part of crtbegin.o that was missing, and only that part
| (crt0 does the rest of crtbegin's job itself). The specs link it
| straight after crt0, so the label below lands at the START of the
| output .eh_frame -- no earlier object has one -- and crtend-eh.o,
| linked last, ends the section with the zero word the unwinder stops
| at.
|
| The reference to __register_frame_info is WEAK: a C program, which
| never pulls the unwinder out of libgcc.a, leaves it undefined, it
| reads as 0, and nothing is registered or linked in. Only a program
| that can throw pays for this.
|
| From a constructor at priority 99, so before any of the program's own
| (101 and up, or none) -- a constructor may throw.

        .section .eh_frame,"a",@progbits
        .p2align 2
__EH_FRAME_BEGIN__:

        .text
        .weak   __register_frame_info
        .type   __sage040_register_eh,@function
__sage040_register_eh:
        move.l  #__register_frame_info,%d0
        beq.s   1f
        move.l  %d0,%a0
        pea     eh_object
        pea     __EH_FRAME_BEGIN__
        jsr     (%a0)
        addq.l  #8,%sp
1:      rts
        .size   __sage040_register_eh, . - __sage040_register_eh

        .section .init_array.00099,"aw"
        .p2align 2
        .long   __sage040_register_eh

| libgcc's struct object, which it keeps for as long as the program
| runs. crtstuff gives it eight words; it uses six.
        .local  eh_object
        .comm   eh_object,32,4

| No executable stack: without this note ld warns, and assumes one.
        .section .note.GNU-stack,"",@progbits
