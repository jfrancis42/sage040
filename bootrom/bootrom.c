/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * bootrom.c - Sage040 boot ROM.
 *
 * Reads a flat image off the ATA disk starting at sector 0, drops it at
 * address 0, and runs it. This is the job a real machine's ROM monitor
 * would do; here it is loaded with QEMU's -kernel, because the Sage040
 * has no ROM.
 *
 * The payload format is deliberately the simplest thing that works: a
 * raw binary with a 68k vector table at its start, exactly as
 * `objcopy -O binary` produces from a kernel linked at address 0. The
 * first two longs of that table are the 68000 reset vectors —
 *
 *     offset 0    initial supervisor stack pointer
 *     offset 4    initial program counter
 *
 * — so the boot ROM does not need to know anything about the payload,
 * not even where it starts. That is how a 68000 boots from ROM, and it
 * means this loader will work unchanged when the payload stops being a
 * demo and becomes a kernel.
 *
 * There is no filesystem and no partition table. Sector 0 is the image.
 */
#include "sage040.h"

/* Where the payload is loaded, and how much of the disk to read. */
#define LOAD_ADDR       0x00000000UL
#define SECTOR_SIZE     512

#ifndef BOOT_SECTORS
#define BOOT_SECTORS    256             /* 128 KB */
#endif

#define LOAD_LIMIT      (LOAD_ADDR + (u32)BOOT_SECTORS * SECTOR_SIZE)

/* The boot ROM itself lives here; the payload must not reach it. */
#define BOOTROM_BASE    0x00200000UL

/* ---------------------------------------------------------------- */
/* ATA, polled PIO                                                   */
/* ---------------------------------------------------------------- */

#define ATA_TIMEOUT     2000000

static int ata_wait_ready(void)
{
    int spin;

    for (spin = 0; spin < ATA_TIMEOUT; spin++) {
        u8 st = MMIO8(ATA_ALTSTAT);

        if (!(st & ATA_SR_BSY)) {
            return (st & ATA_SR_DRDY) ? 0 : -1;
        }
    }
    return -1;
}

static int ata_wait_drq(void)
{
    int spin;

    for (spin = 0; spin < ATA_TIMEOUT; spin++) {
        u8 st = MMIO8(ATA_ALTSTAT);

        if (st & ATA_SR_ERR) {
            return -1;
        }
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) {
            return 0;
        }
    }
    return -1;
}

/*
 * One sector, LBA28.
 *
 * Byte order is worth being careful about. QEMU's MMIO IDE data register
 * is DEVICE_LITTLE_ENDIAN, so a 16-bit read on this big-endian CPU comes
 * back byte-swapped relative to ATA's own word value. But the two bytes
 * of that word, stored big-endian, are exactly the two bytes that sit on
 * the media in that order. So storing the word high byte first
 * reproduces the on-disk byte stream verbatim, which is what a loader
 * needs.
 */
static int ata_read_sector(u32 lba, u8 *dst)
{
    int i;

    if (ata_wait_ready() != 0) {
        return -1;
    }

    MMIO8(ATA_DEVICE) = (u8)(ATA_DEV_LBA | ((lba >> 24) & 0x0f));
    MMIO8(ATA_NSECT)  = 1;
    MMIO8(ATA_LBAL)   = (u8)(lba & 0xff);
    MMIO8(ATA_LBAM)   = (u8)((lba >> 8) & 0xff);
    MMIO8(ATA_LBAH)   = (u8)((lba >> 16) & 0xff);
    MMIO8(ATA_COMMAND) = ATA_CMD_READ_PIO;

    if (ata_wait_drq() != 0) {
        return -1;
    }

    for (i = 0; i < SECTOR_SIZE / 2; i++) {
        u16 w = MMIO16(ATA_DATA);

        dst[i * 2 + 0] = (u8)(w >> 8);
        dst[i * 2 + 1] = (u8)(w & 0xff);
    }

    return (MMIO8(ATA_STATUS) & ATA_SR_ERR) ? -1 : 0;
}

/* ---------------------------------------------------------------- */
/* Handing control over                                              */
/* ---------------------------------------------------------------- */

/*
 * Mask interrupts, point VBR at the payload's own vector table, load its
 * stack pointer and jump.
 *
 * Order matters: the program counter is put in a0 *before* the stack
 * pointer is replaced, because after that instruction there is no stack
 * left to spill anything to.
 */
static void launch(u32 vbr, u32 sp, u32 pc)
{
    __asm__ volatile (
        "move.w  #0x2700,%%sr\n\t"      /* supervisor, interrupts masked */
        "movec   %0,%%vbr\n\t"          /* payload's vector table        */
        "movea.l %2,%%a0\n\t"           /* entry point, before sp goes   */
        "movea.l %1,%%sp\n\t"
        "jmp     (%%a0)\n"
        :
        : "d"(vbr), "a"(sp), "d"(pc)
        : "a0", "memory");

    for (;;) {
        /* not reached */
    }
}

/* ---------------------------------------------------------------- */

/*
 * Held in a volatile so the compiler cannot prove the load address is a
 * null pointer and warn about dereferencing it. Address 0 is a perfectly
 * ordinary place to put a kernel on this machine.
 */
static volatile u32 load_base = LOAD_ADDR;

int main(void)
{
    volatile u32 *image = (volatile u32 *)load_base;
    u8 *load = (u8 *)load_base;
    u32 sp, pc;
    int i;

    uart_init();
    uart_puts("\nSage040 boot ROM\n");
    uart_puts("reading ");
    uart_putdec(BOOT_SECTORS);
    uart_puts(" sectors from LBA 0 to 0x");
    uart_puthex32(LOAD_ADDR);
    uart_puts("\n");

    /* The payload must not land on top of the boot ROM. */
    if (LOAD_LIMIT > BOOTROM_BASE) {
        uart_puts("BOOT: payload window overlaps the boot ROM\n");
        return 0;
    }

    for (i = 0; i < BOOT_SECTORS; i++) {
        if (ata_read_sector((u32)i, load + (u32)i * SECTOR_SIZE) != 0) {
            uart_puts("BOOT: read failed at sector ");
            uart_putdec((u32)i);
            uart_puts("\n");
            return 0;
        }
        if ((i & 63) == 63) {
            uart_putc('.');
        }
    }
    uart_puts("\n");

    /* First two longs of the image are the 68000 reset vectors. */
    sp = image[0];
    pc = image[1];

    uart_puts("image SSP = 0x"); uart_puthex32(sp);
    uart_puts("  PC = 0x");      uart_puthex32(pc);
    uart_puts("\n");

    /*
     * Sanity checks. A blank disk reads back as zeros, which would
     * otherwise send us to address 0 with a null stack and no clue why.
     */
    if (pc < LOAD_ADDR + 8 || pc >= LOAD_LIMIT) {
        uart_puts("BOOT: entry point outside the loaded image"
                  " - is anything written to the disk?\n");
        return 0;
    }
    if (sp < 16 || (sp & 1)) {
        uart_puts("BOOT: implausible stack pointer in the image\n");
        return 0;
    }

    uart_puts("starting\n\n");
    launch(LOAD_ADDR, sp, pc);

    return 0;
}
