/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ata.c - the disk, an ATA taskfile controller in polled PIO mode.
 *
 * Registers itself as the block device "hda".  Everything above it --
 * the filesystem, and through that every file -- sees only a struct
 * blockdev: a sector size, a capacity, and a pair of functions that move
 * sectors.  Replacing this with a SCSI controller or a RAM disk is a new
 * file and one more call in main.c.
 *
 * Reference: T13 ATA/ATAPI specification.  The register file is the
 * WD1003 taskfile carried forward, which is why a driver this small can
 * drive it: LBA28, one command at a time, status polled rather than
 * interrupt-driven.
 *
 * Interrupts are available -- the controller's IRQ reaches MFP channel 6
 * -- and will matter once more than one thing wants the disk at a time.
 * Until there is a scheduler to block, polling is simpler and no slower.
 *
 * ----------------------------------------------------------------------
 * BYTE ORDER, the one thing in this file that is easy to get wrong.
 *
 * Sector contents are a byte stream.  Each 16-bit transfer carries two
 * consecutive bytes of that stream, high byte first, so storing the word
 * big-endian -- which is this CPU's natural order -- reproduces the media
 * exactly.  No swapping.
 *
 * IDENTIFY DEVICE is different.  It returns 16-bit *values*, and QEMU's
 * MMIO data register is little-endian, so a 16-bit read here arrives
 * byte-swapped relative to the value ATA defines.  Those do need a swap.
 *
 * Treating both the same way is self-consistent and therefore passes a
 * write-then-read-back test while writing a byte-swapped image the host
 * cannot read.  That bug lived in the test suite for weeks.
 * ----------------------------------------------------------------------
 */
#include "dev.h"
#include "drivers.h"
#include "errno.h"

#define ATA_TIMEOUT      2000000
#define ATA_SECTOR_SIZE  512

static char  model[41];
static u32   capacity;
static int   ready;

static int ata_wait_notbusy(void)
{
    int spin;

    for (spin = 0; spin < ATA_TIMEOUT; spin++) {
        if (!(MMIO8(ATA_ALTSTAT) & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(void)
{
    int spin;
    u8 st;

    for (spin = 0; spin < ATA_TIMEOUT; spin++) {
        st = MMIO8(ATA_ALTSTAT);
        if (st & (ATA_SR_ERR | ATA_SR_DF)) {
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

/* A sector is a byte stream: store each word high byte first. */
static void ata_pio_in(u8 *dst)
{
    int i;

    for (i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        u16 w = MMIO16(ATA_DATA);
        dst[i * 2 + 0] = (u8)(w >> 8);
        dst[i * 2 + 1] = (u8)(w & 0xff);
    }
}

static void ata_pio_out(const u8 *src)
{
    int i;

    for (i = 0; i < ATA_SECTOR_SIZE / 2; i++) {
        MMIO16(ATA_DATA) = (u16)(((u16)src[i * 2 + 0] << 8) |
                                  (u16)src[i * 2 + 1]);
    }
}

static int ata_identify(void)
{
    u16 id[256];
    int i;

    ready = 0;
    model[0] = '\0';
    capacity = 0;

    if (ata_wait_notbusy() != 0) {
        return -1;
    }

    MMIO8(ATA_DEVICE) = ATA_DEV_LBA;
    if (ata_wait_notbusy() != 0) {
        return -1;
    }
    if (!(MMIO8(ATA_STATUS) & ATA_SR_DRDY)) {
        return -1;
    }

    ata_select_lba(0, 0);
    MMIO8(ATA_COMMAND) = ATA_CMD_IDENTIFY;
    if (ata_wait_drq() != 0) {
        return -1;
    }

    /* IDENTIFY returns word values; undo the little-endian register. */
    for (i = 0; i < 256; i++) {
        u16 w = MMIO16(ATA_DATA);
        id[i] = (u16)((w >> 8) | (w << 8));
    }

    /* Model number, words 27..46, two characters per word. */
    for (i = 0; i < 20; i++) {
        model[i * 2 + 0] = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xff);
    }
    model[40] = '\0';
    for (i = 39; i >= 0 && (model[i] == ' ' || model[i] == '\0'); i--) {
        model[i] = '\0';
    }

    /* Words 60/61: LBA28 sector count, low word first. */
    capacity = (u32)id[60] | ((u32)id[61] << 16);

    ready = 1;
    return 0;
}

static int ata_read(struct blockdev *b, u32 lba, u32 count, void *buf)
{
    u8 *p = buf;
    u32 n;

    (void)b;
    if (!ready || count == 0) {
        return -EINVAL;
    }
    if (lba + count > capacity) {
        return -EIO;
    }

    for (n = 0; n < count; n++) {
        if (ata_wait_notbusy() != 0) {
            return -EIO;
        }
        ata_select_lba(lba + n, 1);
        MMIO8(ATA_COMMAND) = ATA_CMD_READ_PIO;
        if (ata_wait_drq() != 0) {
            return -EIO;
        }
        ata_pio_in(p);
        p += ATA_SECTOR_SIZE;
    }
    return 0;
}

static int ata_write(struct blockdev *b, u32 lba, u32 count,
                     const void *buf)
{
    const u8 *p = buf;
    u32 n;

    (void)b;
    if (!ready || count == 0) {
        return -EINVAL;
    }
    if (lba + count > capacity) {
        return -EIO;
    }

    for (n = 0; n < count; n++) {
        if (ata_wait_notbusy() != 0) {
            return -EIO;
        }
        ata_select_lba(lba + n, 1);
        MMIO8(ATA_COMMAND) = ATA_CMD_WRITE_PIO;
        if (ata_wait_drq() != 0) {
            return -EIO;
        }
        ata_pio_out(p);
        p += ATA_SECTOR_SIZE;

        /*
         * Wait for the drive to take the data before issuing the next
         * command, and check that it did: a write that fails silently is
         * a filesystem that corrupts itself much later.
         */
        if (ata_wait_notbusy() != 0) {
            return -EIO;
        }
        if (MMIO8(ATA_STATUS) & (ATA_SR_ERR | ATA_SR_DF)) {
            return -EIO;
        }
    }
    return 0;
}

/*
 * The device this driver presents upward.  Nothing above it knows the
 * word "ATA": the filesystem asks a struct blockdev for sectors.
 */
static struct blockdev ata_dev = {
    "hda",
    model,
    ATA_SECTOR_SIZE,
    0,
    ata_read,
    ata_write,
    0,
    0
};

int ata_init(void)
{
    /* Is the chip fitted? An address with nothing behind it raises a
     * bus error rather than reading back zeroes, so this has to be
     * asked before the first register access, not by making one. */
    if (!io_probe8((volatile void *)ATA_ALTSTAT)) {
        return -ENODEV;
    }

    if (ata_identify() != 0) {
        return -ENODEV;
    }
    ata_dev.sectors = capacity;
    ata_dev.model = model;
    return dev_register_block(&ata_dev);
}
