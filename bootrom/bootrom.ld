/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * bootrom.ld - linker script for the Sage040 boot ROM.
 *
 * The boot ROM loads its payload to address 0, so it cannot live there
 * itself. It is linked at 2 MB, leaving the whole bottom 2 MB of RAM
 * free for whatever it loads, and takes its stack from the top of RAM.
 *
 *   0x00000000 - 0x001fffff   payload (loaded from disk)
 *   0x00200000 - 0x003fffff   boot ROM, then its stack at the top
 */
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x00200000, LENGTH = 2M
}

_stack_top = ORIGIN(RAM) + LENGTH(RAM) - 16;

SECTIONS {
    .vectors 0x00200000 : { KEEP(*(.vectors)) } > RAM
    .text : {
        *(.text .text.*)
    } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data   : { *(.data .data.*) }     > RAM
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
