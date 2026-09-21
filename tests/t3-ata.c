/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * t3-ata.c - ATA taskfile disk controller, PIO mode.
 *
 * Reference: T13 ATA/ATAPI specification (the register file is the
 * WD1003 taskfile that MFM controllers used, carried forward).
 *
 * Sequence under test:
 *   IDENTIFY DEVICE  -> model string and capacity
 *   WRITE SECTORS    -> a known pattern to an LBA
 *   READ SECTORS     -> read it back and compare
 *
 * No DMA, no interrupts: status polling only, which is all a simple OS
 * needs and is the easiest possible disk driver.
 */
#include "sage040.h"

static u16 idbuf[256];
static u8  secbuf[512];
static u8  cmpbuf[512];

/* Wait for BSY to clear.  Returns 0 on success, -1 on timeout. */
static int ata_wait_notbusy(void)
{
    int spin;

    for (spin = 0; spin < 2000000; spin++) {
        if (!(MMIO8(ATA_ALTSTAT) & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

/* Wait for DRQ (data request).  Returns 0 on success, -1 otherwise. */
static int ata_wait_drq(void)
{
    int spin;
    u8 st;

    for (spin = 0; spin < 2000000; spin++) {
        st = MMIO8(ATA_ALTSTAT);
        if (st & ATA_SR_ERR) {
            return -1;
        }
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) {
            return 0;
        }
    }
    return -1;
}

static void ata_select_lba(u32 lba, u8 count)
{
    MMIO8(ATA_DEVICE) = (u8)(ATA_DEV_LBA | ((lba >> 24) & 0x0f));
    MMIO8(ATA_NSECT)  = count;
    MMIO8(ATA_LBAL)   = (u8)(lba & 0xff);
    MMIO8(ATA_LBAM)   = (u8)((lba >> 8) & 0xff);
    MMIO8(ATA_LBAH)   = (u8)((lba >> 16) & 0xff);
}

/*
 * Sector data is a byte stream, and the two bytes of each transferred
 * word -- stored high byte first -- are exactly the two bytes that sit
 * on the media in that order.  So a plain big-endian store reproduces
 * the on-disk stream verbatim.
 *
 * This is NOT the same as the IDENTIFY handling below.  IDENTIFY returns
 * 16-bit *values*, and those do need swapping, because QEMU's MMIO IDE
 * data register is DEVICE_LITTLE_ENDIAN and a 16-bit read on this
 * big-endian CPU comes back byte-swapped relative to ATA's word value.
 * Byte streams and word values want opposite treatment; conflating them
 * writes a byte-swapped image to the disk that still passes a
 * write-then-read-back test, because both directions swap.
 */
static void ata_read_sector_bytes(u8 *dst)
{
    int i;

    for (i = 0; i < 256; i++) {
        u16 w = MMIO16(ATA_DATA);
        dst[i * 2 + 0] = (u8)(w >> 8);
        dst[i * 2 + 1] = (u8)(w & 0xff);
    }
}

static void ata_write_sector_bytes(const u8 *src)
{
    int i;

    for (i = 0; i < 256; i++) {
        u16 w = ((u16)src[i * 2 + 0] << 8) | (u16)src[i * 2 + 1];
        MMIO16(ATA_DATA) = w;
    }
}

int main(void)
{
    int i;
    u8 st;

    test_begin("t3 ATA taskfile disk");

    /* --- the controller should be present and not busy --- */
    if (ata_wait_notbusy() == 0) {
        test_ok("controller responds, BSY clear");
    } else {
        test_fail("controller BSY never cleared - no disk attached?");
        test_end();
        return 0;
    }

    MMIO8(ATA_DEVICE) = ATA_DEV_LBA;        /* select master, LBA mode */
    ata_wait_notbusy();

    st = MMIO8(ATA_STATUS);
    uart_puts("  STATUS      = 0x"); uart_puthex8(st); uart_putc('\n');
    if (st & ATA_SR_DRDY) {
        test_ok("device reports DRDY");
    } else {
        test_fail("device never became ready");
    }

    /* --- IDENTIFY DEVICE --- */
    ata_select_lba(0, 0);
    MMIO8(ATA_COMMAND) = ATA_CMD_IDENTIFY;

    if (ata_wait_drq() != 0) {
        test_fail("IDENTIFY did not produce DRQ");
        test_end();
        return 0;
    }
    test_ok("IDENTIFY DEVICE returned data");

    /*
     * The data register region is DEVICE_LITTLE_ENDIAN, so a 16-bit read
     * on this big-endian CPU arrives byte-swapped.  Undo that here so
     * idbuf[] holds words in ATA's own order and the numeric fields can
     * be used directly.
     */
    for (i = 0; i < 256; i++) {
        u16 w = MMIO16(ATA_DATA);
        idbuf[i] = (u16)((w >> 8) | (w << 8));
    }

    /*
     * Model number is words 27..46.  ATA stores each pair of characters
     * byte-swapped within the word, so with words now in native order the
     * first character is the high byte.
     */
    uart_puts("  model       = '");
    for (i = 27; i <= 46; i++) {
        char a = (char)(idbuf[i] >> 8);
        char b = (char)(idbuf[i] & 0xff);
        if (a >= 32 && a < 127) uart_putc(a);
        if (b >= 32 && b < 127) uart_putc(b);
    }
    uart_puts("'\n");

    /* Words 60/61 hold the LBA28 sector count, low word first. */
    {
        u32 sectors = (u32)idbuf[60] | ((u32)idbuf[61] << 16);
        uart_puts("  LBA28 size  = "); uart_putdec(sectors);
        uart_puts(" sectors ("); uart_putdec(sectors / 2048);
        uart_puts(" MiB)\n");
        if (sectors == 16384) {
            test_ok("IDENTIFY reports exactly 8 MiB, matching disk.img");
        } else if (sectors > 0) {
            test_ok("IDENTIFY reports a non-zero capacity");
        } else {
            test_fail("IDENTIFY reports zero capacity");
        }
    }

    /* --- write a known pattern to LBA 1 --- */
    for (i = 0; i < 512; i++) {
        secbuf[i] = (u8)(i ^ 0x5A);
    }
    secbuf[0] = 'S'; secbuf[1] = 'A'; secbuf[2] = 'G'; secbuf[3] = 'E';

    ata_wait_notbusy();
    ata_select_lba(1, 1);
    MMIO8(ATA_COMMAND) = ATA_CMD_WRITE_PIO;
    if (ata_wait_drq() != 0) {
        test_fail("WRITE SECTORS did not request data");
        test_end();
        return 0;
    }
    ata_write_sector_bytes(secbuf);
    ata_wait_notbusy();

    st = MMIO8(ATA_STATUS);
    if (st & ATA_SR_ERR) {
        test_fail("WRITE SECTORS reported an error");
    } else {
        test_ok("WRITE SECTORS to LBA 1 completed");
    }

    /* --- read it back --- */
    for (i = 0; i < 512; i++) {
        cmpbuf[i] = 0;
    }

    ata_wait_notbusy();
    ata_select_lba(1, 1);
    MMIO8(ATA_COMMAND) = ATA_CMD_READ_PIO;
    if (ata_wait_drq() != 0) {
        test_fail("READ SECTORS did not produce DRQ");
        test_end();
        return 0;
    }
    ata_read_sector_bytes(cmpbuf);
    ata_wait_notbusy();

    uart_puts("  first 8 read= ");
    for (i = 0; i < 8; i++) {
        uart_puthex8(cmpbuf[i]); uart_putc(' ');
    }
    uart_putc('\n');

    {
        int bad = -1;

        for (i = 0; i < 512; i++) {
            if (cmpbuf[i] != secbuf[i]) {
                bad = i;
                break;
            }
        }
        if (bad < 0) {
            test_ok("read-back of all 512 bytes matches what was written");
        } else {
            uart_puts("  mismatch at byte "); uart_putdec((u32)bad);
            uart_puts(": wrote 0x"); uart_puthex8(secbuf[bad]);
            uart_puts(" read 0x"); uart_puthex8(cmpbuf[bad]);
            uart_putc('\n');
            test_fail("read-back mismatch");
        }
    }

    /*
     * Round-tripping our own writes proves consistency, not correctness:
     * a driver that swaps on the way in and out again passes it while
     * putting a byte-swapped image on the media.  So read a signature the
     * *host* wrote into the image before boot (see runtest.sh) and check
     * it byte for byte.  That is the only way to catch absolute byte
     * order from inside the guest.
     */
    {
        static const char sig[] = "SAGE040-DISK-OK!";
        int bad = -1;

        ata_wait_notbusy();
        ata_select_lba(2, 1);
        MMIO8(ATA_COMMAND) = ATA_CMD_READ_PIO;
        if (ata_wait_drq() != 0) {
            test_fail("could not read the host-written signature sector");
        } else {
            ata_read_sector_bytes(cmpbuf);
            ata_wait_notbusy();

            for (i = 0; i < 16; i++) {
                if (cmpbuf[i] != (u8)sig[i]) {
                    bad = i;
                    break;
                }
            }
            uart_puts("  host signature = '");
            for (i = 0; i < 16; i++) {
                char c = (char)cmpbuf[i];
                uart_putc((c >= 32 && c < 127) ? c : '?');
            }
            uart_puts("'\n");

            if (bad < 0) {
                test_ok("on-disk byte order matches what the host wrote");
            } else {
                test_fail("on-disk byte order WRONG - bytes are swapped");
            }
        }
    }

    test_end();
    return 0;
}
