/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ata.c - the disk, an ATA taskfile controller in PIO mode.
 *
 * Registers itself as the block device "hda".  Everything above it --
 * the filesystem, and through that every file -- sees only a struct
 * blockdev: a sector size, a capacity, and a pair of functions that move
 * sectors.  Replacing this with a SCSI controller or a RAM disk is a new
 * file and one more call in main.c.
 *
 * Reference: T13 ATA/ATAPI specification.  The register file is the
 * WD1003 taskfile carried forward, which is why a driver this small can
 * drive it: LBA28, one command at a time.
 *
 * INTERRUPT-DRIVEN (task 22). The controller's IRQ reaches MFP GPIP4.
 * A command moves up to 256 sectors, and between one sector and the
 * next the drive raises its interrupt: the task that asked SLEEPS until
 * it does, and the machine runs something else. It used to spin on the
 * status register for the whole of every transfer, and with a kernel
 * that is not preempted that stopped every task for as long as the disk
 * took -- short under QEMU, milliseconds on a real drive.
 *
 * Where nothing may sleep -- at boot, before the scheduler, in the idle
 * task -- the same waits poll instead. Either way a wait has a limit,
 * so a dead drive is an error, not a hang.
 *
 * One request at a time: a task sleeping in the middle of a transfer
 * would otherwise have another task's command written over its own.
 * The filesystem has a lock of its own above this (vfs.c); swap I/O
 * comes here without it.
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
#include "task.h"
#include "wait.h"
#include "errno.h"

#define ATA_TIMEOUT      2000000
#define ATA_SECTOR_SIZE  512

static char  model[41];
static u32   capacity;
static int   ready;

#define ATA_NIEN         0x02           /* device control: interrupts off */
#define ATA_WAIT_MS      5000           /* a dead drive, not a slow one   */

static int            irq_on;           /* the interrupt is installed     */
static volatile int   irq_seen;         /* set by the handler             */
static volatile u8    irq_status;       /* the status it read             */
static struct waitq   irq_wait;
static struct mutex   ata_lock;
static u32            waits_slept, waits_polled;

/*
 * A test knob (kstat KSTAT_DISK_DELAY): every request waits this many
 * milliseconds first, asleep, before it starts. Real disks are slow and
 * QEMU's is not, so without it the moments when one task is asleep
 * half way through a filesystem operation are too short for a test to
 * land another task in -- and a test that cannot fail without the
 * filesystem lock proves nothing about it.
 */
static u32            delay_ms;
static struct waitq   delay_wait;

void ata_set_delay(u32 ms)
{
    delay_ms = ms;
}

static void test_delay(void)
{
    if (delay_ms && task_can_sleep()) {
        sleep_on_timeout(&delay_wait, delay_ms);
    }
}

/*
 * The interrupt. Reading STATUS -- not ALTSTAT -- is what acknowledges
 * it and drops the line, which the MFP needs: it sees edges.
 */
static void ata_isr(void *arg)
{
    (void)arg;
    irq_status = MMIO8(ATA_STATUS);
    irq_seen = 1;
    wake_all(&irq_wait);
}

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

/*
 * Wait for the drive to finish the phase just started -- a sector ready
 * to read, or one written -- and return its status, or -1 after
 * ATA_WAIT_MS. The caller cleared irq_seen BEFORE starting the phase, so
 * an interrupt that comes first is not missed.
 */
static int ata_wait_phase(void)
{
    if (irq_on && task_can_sleep()) {
        u32 waited = 0;

        waits_slept++;
        while (!irq_seen) {
            if (waited >= ATA_WAIT_MS) {
                return -1;
            }
            sleep_on_timeout(&irq_wait, 50);
            waited += 50;
        }
        irq_seen = 0;
        return irq_status;
    }
    waits_polled++;
    if (ata_wait_notbusy() != 0) {
        return -1;
    }
    irq_seen = 0;
    return MMIO8(ATA_STATUS);           /* acknowledges, if it was on */
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
    int err = 0;

    (void)b;
    if (!ready || count == 0) {
        return -EINVAL;
    }
    if (lba + count > capacity) {
        return -EIO;
    }

    test_delay();
    mutex_lock(&ata_lock);
    while (count > 0 && !err) {
        u32 n = count > 256 ? 256 : count, k;

        if (ata_wait_notbusy() != 0) {
            err = -EIO;
            break;
        }
        irq_seen = 0;
        ata_select_lba(lba, (u8)(n & 0xff));        /* 0 means 256 */
        MMIO8(ATA_COMMAND) = ATA_CMD_READ_PIO;
        for (k = 0; k < n; k++) {
            int st = ata_wait_phase();

            if (st < 0 || (st & (ATA_SR_ERR | ATA_SR_DF)) ||
                !(st & ATA_SR_DRQ)) {
                err = -EIO;
                break;
            }
            ata_pio_in(p);
            p += ATA_SECTOR_SIZE;
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&ata_lock);
    return err;
}

static int ata_write(struct blockdev *b, u32 lba, u32 count,
                     const void *buf)
{
    const u8 *p = buf;
    int err = 0;

    (void)b;
    if (!ready || count == 0) {
        return -EINVAL;
    }
    if (lba + count > capacity) {
        return -EIO;
    }

    test_delay();
    mutex_lock(&ata_lock);
    while (count > 0 && !err) {
        u32 n = count > 256 ? 256 : count, k;

        if (ata_wait_notbusy() != 0) {
            err = -EIO;
            break;
        }
        ata_select_lba(lba, (u8)(n & 0xff));
        MMIO8(ATA_COMMAND) = ATA_CMD_WRITE_PIO;
        /* The first sector is asked for with DRQ and no interrupt; each
         * one after it, and the end, with an interrupt. */
        if (ata_wait_drq() != 0) {
            err = -EIO;
            break;
        }
        for (k = 0; k < n; k++) {
            int st;

            irq_seen = 0;
            ata_pio_out(p);
            p += ATA_SECTOR_SIZE;
            st = ata_wait_phase();
            /*
             * Checked after every sector: a write that fails silently is
             * a filesystem that corrupts itself much later.
             */
            if (st < 0 || (st & (ATA_SR_ERR | ATA_SR_DF)) ||
                (k + 1 < n && !(st & ATA_SR_DRQ))) {
                err = -EIO;
                break;
            }
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&ata_lock);
    return err;
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
    mutex_init(&ata_lock);
    MMIO8(ATA_DEVCTL) = ATA_NIEN;       /* until ata_irq_on() */
    return dev_register_block(&ata_dev);
}

/*
 * The interrupt, once the MFP is up -- the disk is found before it, and
 * mfp_init() clears every handler and enable. nIEN clear lets the drive
 * raise it. Without this every wait polls, as it always did.
 */
int ata_irq_on(void)
{
    int err;

    if (!ready) {
        return -ENODEV;
    }
    err = mfp_request_gpip(MFP_PIN_ATA, ata_isr, 0);
    if (err < 0) {
        return err;
    }
    MMIO8(ATA_DEVCTL) = 0;
    (void)MMIO8(ATA_STATUS);            /* anything pending, cleared */
    irq_on = 1;
    return 0;
}

void ata_counts(u32 *slept, u32 *polled)
{
    *slept = waits_slept;
    *polled = waits_polled;
}
