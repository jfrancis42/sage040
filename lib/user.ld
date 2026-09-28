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
 *   ...         heap, then an unmapped gap, then mmap() space
 *   0x1ff00000  its stack, 1 MB, growing down
 *   0x20000000  end of the address space
 *
 * It is up here, rather than at the 1 MB it used to sit at, so that the
 * kernel cannot reach it by accident. The kernel identity-maps RAM, so a
 * program at 1 MB would be at an address the kernel could dereference --
 * and a system call that forgot to go through uaccess would quietly read
 * kernel memory instead of failing. At 0x10000000 there is nothing there
 * in the kernel's map and the same mistake is an immediate fault.
 *
 * The address below and kernel/vm.h have to agree, and so does the
 * length: everything up to USER_BRK_LIMIT, the guard page below the
 * stack. It said 2M - 64K for a while after the address space grew to
 * 256 MB, which would have refused to link any program bigger than
 * about 1.9 MB with nothing to say why.
 *
 * No vector table and no reset vectors: a program is loaded by name and
 * entered at its ELF entry point, not found at a fixed address by a ROM.
 */
ENTRY(_start)

MEMORY {
    USER (rwx) : ORIGIN = 0x10000000, LENGTH = 256M - 1M - 4K
}

SECTIONS {
    .text 0x10000000 : {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > USER
    .rodata : { *(.rodata .rodata.*) } > USER
    /*
     * Everything writable starts on a page of its own. Under QEMU, a
     * store to a page holding code it has translated throws that
     * translation away, so a global written in a loop that shares a
     * page with the loop's own code has it retranslated on every pass.
     * wator's frame took 26 ms instead of 1.5 for exactly that: its row
     * buffer sat in .bss on the last page of .text. Which programs it
     * hit depended on where the code happened to end. Real hardware has
     * no such cost; it costs a page per program either way.
     *
     * The alignment is the section's address, not a `. = ALIGN(4096)`
     * before it: with `> USER`, ld places a section at the region's
     * next free byte and ignores the assignment -- silently. And the
     * `. = ALIGN(4)` inside is what keeps the section when a program
     * has no .data (ld keeps a section that assigns to `.`, but not one
     * that only defines a symbol), so that .bss still lands on the new
     * page rather than straight after .rodata.
     *
     * .bss says `. :` for the same reason. Without an address it too is
     * put at the region's next free byte, and an EMPTY section placed
     * at an explicit address does not move that on: `hello`, which has
     * no .data, had its .bss back on the page of its code. `.` does
     * follow the section before, empty or not.
     */
    .data ALIGN(4096) : { . = ALIGN(4); *(.data .data.*) } > USER
    .bss . : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > USER
    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
