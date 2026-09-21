/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * user.ld - linker script for programs.
 *
 * A program is linked at 256 MB, IN ITS OWN ADDRESS SPACE. Every program
 * has the same address, because no two of them can see each other: the
 * number below is virtual and the physical pages behind it come from
 * wherever the allocator had some.
 *
 *   0x10000000  this program        <- USER_VA_BASE in kernel/vm.h
 *   ...         unmapped gap -- a runaway stack faults here
 *   0x101ffff0  its stack, growing down
 *   0x10200000  end of the address space
 *
 * It is up here, rather than at the 1 MB it used to sit at, so that the
 * kernel cannot reach it by accident. The kernel identity-maps RAM, so a
 * program at 1 MB would be at an address the kernel could dereference --
 * and a system call that forgot to go through uaccess would quietly read
 * kernel memory instead of failing. At 0x10000000 there is nothing there
 * in the kernel's map and the same mistake is an immediate fault.
 *
 * The address below and kernel/vm.h have to agree.
 *
 * No vector table and no reset vectors: a program is loaded by name and
 * entered at its ELF entry point, not found at a fixed address by a ROM.
 */
ENTRY(_start)

MEMORY {
    USER (rwx) : ORIGIN = 0x10000000, LENGTH = 2M - 64K
}

SECTIONS {
    .text 0x10000000 : {
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
