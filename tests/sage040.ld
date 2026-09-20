/*
 * sage040.ld - linker script for the Sage040 machine.
 *
 * Physical memory map (see hw/m68k/sage040.c):
 *   0x00000000  RAM
 *   0xff000000  NS16550A UART
 *   0xff100000  ATA command block   0xff101000 ATA control block
 *   0xff200000  LAN91C111
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
