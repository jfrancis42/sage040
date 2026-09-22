/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sage040.ld - linker script for programs built with picolibc.
 *
 * The same address space as lib/user.ld -- a program at 256 MB, in its
 * own address space, with everything up to the guard page below its
 * stack to grow into -- plus what a C library needs that a program on
 * lib/ulib does not: the constructor and destructor arrays, with the
 * symbols __libc_init_array and exit() walk.
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
     * One section for both, the way picolibc's own scripts lay it out:
     * __libc_init_array runs everything from __bothinit_array_start to
     * __bothinit_array_end in a single loop, preinit first. With the
     * two in separate sections and only the separate symbols defined,
     * it found neither and ran no constructors at all.
     */
    .init_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        PROVIDE_HIDDEN(__bothinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)
               SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__bothinit_array_end = .);
    } > USER
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        /* .dtors.*: this gcc puts a destructor WITH A PRIORITY there,
         * not in .fini_array.* -- picolibc's stdout flush at exit is
         * one, and without this it was discarded and never ran. Line
         * buffering hid it until stdout was fully buffered into files:
         * only output with no final newline was ever lost. */
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)
               SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        /* .fini_array_onexit: picolibc's pointer to the function that
         * runs atexit() handlers. exit() only walks this array, so
         * without it no atexit handler ever ran. */
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > USER

    /*
     * THE GLOBAL OFFSET TABLE, and it has to be named here.
     *
     * With no rule for it, ld places .got and .got.plt as ORPHAN
     * sections -- near sections with similar attributes, which is not
     * the same as together -- and defines _GLOBAL_OFFSET_TABLE_ from
     * where it decided to put one of them. Code then reads its GOT
     * entries at offsets that land past the end of the table, in .bss,
     * where they are zero. Every address a position-independent object
     * fetches from the GOT comes back null.
     *
     * Nothing noticed for a long time because only a STATIC program
     * uses this script and almost nothing static here is built -fPIC.
     * CPython compiles its modules that way, and died dereferencing a
     * null module definition. The order below is ld's own default.
     */
    .got : {
        *(.got.plt) *(.igot.plt) *(.got) *(.igot)
    } > USER
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } > USER

    .data : { *(.data .data.*) } > USER
    .bss  : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > USER
    _end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
