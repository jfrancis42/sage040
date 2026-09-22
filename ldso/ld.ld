/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ld.ld - where the dynamic linker lives.
 *
 * At a FIXED address, as an ET_EXEC, which Linux also accepts as an
 * interpreter. That is what lets it run without relocating itself: a
 * position-independent ld.so has to fix up its own GOT before it can
 * touch a global, with code written to use none. Here it simply is
 * where it was linked.
 *
 * 0x1fe00000 is the megabyte below the stack's guard page. brk refuses
 * to grow over a mapped page and mmap's search skips them, so neither
 * can land on it. The libraries go wherever mmap finds room, searching
 * down from the stack -- which is first the rest of this megabyte,
 * above ld.so itself, and then below it. The kernel refuses an
 * interpreter that is not wholly inside [LDSO_BASE, LDSO_END) (exec.c).
 */
ENTRY(_dl_start)

SECTIONS {
    .text 0x1fe00000 : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    .data ALIGN(4096) : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
