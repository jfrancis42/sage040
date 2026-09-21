/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * kernel.ld - linker script for the Sage040 kernel.
 *
 * The kernel owns low memory. Its vector table sits at address 0, which
 * is both where the 68040 looks after reset and where the boot ROM finds
 * the initial SSP and PC before jumping here.
 *
 *   0x00000000  vector table, 1 KB
 *   0x00000400  kernel text, rodata, data, bss
 *   ...
 *   0x003ffff0  supervisor stack, growing down
 *
 * The boot ROM links at 2 MB and is dead by the time this runs, so the
 * stack may sit above it.
 */
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 4M
}

_stack_top = ORIGIN(RAM) + LENGTH(RAM) - 16;

SECTIONS {
    .vectors 0x00000000 : { KEEP(*(.vectors)) } > RAM
    .text 0x00000400 : {
        *(.text .text.*)
    } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data   : { *(.data .data.*) } > RAM
    .bss    : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM
    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
