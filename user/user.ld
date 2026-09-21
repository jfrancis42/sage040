/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * user.ld - linker script for programs.
 *
 * A program is linked at 1 MB, above the kernel and below the stack the
 * kernel hands it. It is an ordinary ELF executable from here on: the
 * kernel reads the program headers and puts each segment where this
 * script said to, so the address below and kernel/exec.h have to agree.
 *
 *   0x00000000  kernel
 *   0x00100000  this program        <- USER_BASE
 *   0x002ffff0  its stack, growing down
 *   0x003ffff0  kernel supervisor stack
 *
 * No vector table and no reset vectors: a program is loaded by name and
 * entered at its ELF entry point, not found at a fixed address by a ROM.
 */
ENTRY(_start)

MEMORY {
    USER (rwx) : ORIGIN = 0x00100000, LENGTH = 2M - 64K
}

SECTIONS {
    .text 0x00100000 : {
        *(.text.start)
        *(.text .text.*)
    } > USER
    .rodata : { *(.rodata .rodata.*) } > USER
    .data   : { *(.data .data.*) }     > USER
    .bss    : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > USER
    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
