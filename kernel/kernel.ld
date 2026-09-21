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
 *              the boot supervisor stack, 64 KB
 *   _end       everything above here belongs to the page allocator
 *
 * THE STACK IS PART OF THE IMAGE, not a fixed address near the top of
 * memory. It used to be at 4 MB minus 16, with LENGTH below saying 4M,
 * and that quietly capped the machine: the page allocator runs from
 * `_end` to just under the stack, so every byte of RAM above 4 MB was
 * invisible no matter how much the emulator was given. Putting the
 * stack in the image means the allocator gets everything above it and
 * the kernel does not have to know how much that is.
 *
 * LENGTH bounds the IMAGE, not the machine. Real memory is sized at run
 * time by probe_memory().
 *
 * The boot ROM links at 2 MB and is dead by the time this runs.
 */
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 4M
}

/* The boot supervisor stack, reserved inside the image. 64 KB: traps,
 * the exception reporter and the early console all run on it, and it is
 * only used until each task gets its own. */
KSTACK_SIZE = 64K;

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

    .kstack : {
        . = ALIGN(4096);
        _stack_bottom = .;
        . += KSTACK_SIZE;
        . = ALIGN(16);
        _stack_top = . - 16;
    } > RAM

    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
