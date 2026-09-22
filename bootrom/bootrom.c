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
 * Disk layout. The disk is a real MS-DOS disk, so the host can read and
 * write it with ordinary tools — mtools, mount -t vfat, fdisk, fsck.fat:
 *
 *     LBA 0             MBR partition table
 *     LBA 64            optional raw kernel, in the boot gap
 *     LBA 2048          partition 1, FAT16 filesystem
 *
 * The normal path is to find KERNEL.ROM in the root directory of that
 * filesystem and load it. That is the point of using a real DOS format:
 * replacing the kernel is `mcopy kernel.rom ::/`, not `dd`.
 *
 * If there is no filesystem, or no KERNEL.ROM in it, the loader falls
 * back to reading a raw image from LBA 64 — the gap between the
 * partition table and the first partition, which is the classic place to
 * put a boot image and is nearly a megabyte here. That keeps a
 * filesystem-less disk bootable.
 *
 * Only what is needed to find one file in the root directory is
 * implemented: FAT16, 8.3 names, no subdirectories, no long names, read
 * only. Everything in FAT is little-endian and this machine is not, so
 * every multi-byte field goes through le16()/le32().
 */
#include "sage040.h"

/* Where the payload is loaded, and how much of the disk to read. */
#define LOAD_ADDR       0x00000000UL
#define SECTOR_SIZE     512

/* Disk layout. */
#define MBR_LBA         0
#define KERNEL_LBA      64              /* first kernel sector, in the gap */

/* MBR field offsets, all little-endian on disk. */
#define MBR_PART0       446             /* first of four 16-byte entries   */
#define MBR_PART_TYPE   4
#define MBR_PART_LBA    8
#define MBR_SIG         510             /* 0x55 0xAA                       */

/*
 * The most the payload may be. It was 256 sectors, 128 KB, and the
 * kernel passed that the day /bin/sh's wrappers moved into it -- the ROM
 * said "short read" and nothing booted. 1920 sectors is 960 KB: the
 * most the raw fallback can read from the gap before a partition at LBA
 * 2048, and far below the ROM itself at 2 MB. Loading KERNEL.ROM from
 * the filesystem reads only the file's own size, so this costs a normal
 * boot nothing.
 */
#ifndef BOOT_SECTORS
#define BOOT_SECTORS    1920            /* 960 KB */
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
/* Partition table                                                   */
/* ---------------------------------------------------------------- */

static u8 mbr[SECTOR_SIZE];

/* MBR fields are little-endian; sector bytes reach us in media order. */
static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/*
 * Returns the first sector of partition 1, or 0 if the disk carries no
 * usable partition table. Zero is never a valid partition start, so it
 * doubles as "not partitioned".
 */
static u32 partition_start(void)
{
    if (ata_read_sector(MBR_LBA, mbr) != 0) {
        return 0;
    }
    if (mbr[MBR_SIG] != 0x55 || mbr[MBR_SIG + 1] != 0xAA) {
        return 0;
    }
    if (mbr[MBR_PART0 + MBR_PART_TYPE] == 0x00) {
        return 0;                       /* entry unused */
    }
    return le32(&mbr[MBR_PART0 + MBR_PART_LBA]);
}

/* ---------------------------------------------------------------- */
/* FAT16, read only, enough to find one file in the root directory   */
/* ---------------------------------------------------------------- */

/* BPB field offsets within the partition's first sector. */
#define BPB_BYTES_PER_SEC   11
#define BPB_SEC_PER_CLUS    13
#define BPB_RSVD_SEC_CNT    14
#define BPB_NUM_FATS        16
#define BPB_ROOT_ENT_CNT    17
#define BPB_FAT_SZ16        22

#define DIR_ENTRY_SIZE      32
#define DIR_NAME            0
#define DIR_ATTR            11
#define DIR_FST_CLUS_LO     26
#define DIR_FILE_SIZE       28

#define ATTR_VOLUME_ID      0x08
#define ATTR_DIRECTORY      0x10
#define ATTR_LONG_NAME      0x0f

#define FAT16_EOC           0xfff8      /* >= this ends a chain */

static u8 secbuf[SECTOR_SIZE];          /* BPB and directory sectors */
static u8 fatbuf[SECTOR_SIZE];          /* one cached FAT sector     */
static u32 fat_cached = 0xffffffffUL;

struct fat_info {
    u32 fat_start;          /* LBA of the first FAT                */
    u32 root_start;         /* LBA of the root directory           */
    u32 data_start;         /* LBA of cluster 2                    */
    u32 root_entries;
    u32 sec_per_clus;
};

static u16 le16(const u8 *p)
{
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

static int fat_mount(u32 part_lba, struct fat_info *fi)
{
    u32 bps, rsvd, nfats, fatsz, root_sectors;

    if (ata_read_sector(part_lba, secbuf) != 0) {
        return -1;
    }

    bps   = le16(&secbuf[BPB_BYTES_PER_SEC]);
    rsvd  = le16(&secbuf[BPB_RSVD_SEC_CNT]);
    nfats = secbuf[BPB_NUM_FATS];
    fatsz = le16(&secbuf[BPB_FAT_SZ16]);

    fi->sec_per_clus = secbuf[BPB_SEC_PER_CLUS];
    fi->root_entries = le16(&secbuf[BPB_ROOT_ENT_CNT]);

    /* Only the shape this loader can actually handle. */
    if (bps != SECTOR_SIZE || fi->sec_per_clus == 0 ||
        rsvd == 0 || nfats == 0 || fatsz == 0 || fi->root_entries == 0) {
        return -1;
    }

    root_sectors = (fi->root_entries * DIR_ENTRY_SIZE + bps - 1) / bps;

    fi->fat_start  = part_lba + rsvd;
    fi->root_start = fi->fat_start + nfats * fatsz;
    fi->data_start = fi->root_start + root_sectors;
    return 0;
}

/* Next cluster in the chain, with a one-sector cache for sequential runs. */
static u16 fat_next(const struct fat_info *fi, u16 clus)
{
    u32 off = (u32)clus * 2;
    u32 sec = fi->fat_start + off / SECTOR_SIZE;

    if (sec != fat_cached) {
        if (ata_read_sector(sec, fatbuf) != 0) {
            return FAT16_EOC;
        }
        fat_cached = sec;
    }
    return le16(&fatbuf[off % SECTOR_SIZE]);
}

/* Compare a directory entry's 8.3 field against a padded 11-byte name. */
static int name_matches(const u8 *entry, const char *want11)
{
    int i;

    for (i = 0; i < 11; i++) {
        if (entry[DIR_NAME + i] != (u8)want11[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * Find a file in the root directory. Returns its first cluster, or 0 if
 * it is not there; *size is set to the file length.
 */
static u16 fat_find(const struct fat_info *fi, const char *name11, u32 *size)
{
    u32 per_sector = SECTOR_SIZE / DIR_ENTRY_SIZE;
    u32 sectors = (fi->root_entries + per_sector - 1) / per_sector;
    u32 s, e;

    for (s = 0; s < sectors; s++) {
        if (ata_read_sector(fi->root_start + s, secbuf) != 0) {
            return 0;
        }
        for (e = 0; e < per_sector; e++) {
            const u8 *d = &secbuf[e * DIR_ENTRY_SIZE];
            u8 attr = d[DIR_ATTR];

            if (d[DIR_NAME] == 0x00) {
                return 0;               /* end of directory */
            }
            if (d[DIR_NAME] == 0xe5) {
                continue;               /* deleted */
            }
            if ((attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
                continue;               /* long-name fragment */
            }
            if (attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) {
                continue;
            }
            if (name_matches(d, name11)) {
                *size = (u32)le16(&d[DIR_FILE_SIZE]) |
                        ((u32)le16(&d[DIR_FILE_SIZE + 2]) << 16);
                return le16(&d[DIR_FST_CLUS_LO]);
            }
        }
    }
    return 0;
}

/* Walk the cluster chain, copying the file to dst. Returns bytes read. */
static u32 fat_load(const struct fat_info *fi, u16 clus, u32 size, u8 *dst)
{
    u32 done = 0;

    while (clus >= 2 && clus < FAT16_EOC && done < size) {
        u32 lba = fi->data_start + (u32)(clus - 2) * fi->sec_per_clus;
        u32 i;

        for (i = 0; i < fi->sec_per_clus && done < size; i++) {
            if (done + SECTOR_SIZE > LOAD_LIMIT - LOAD_ADDR) {
                return done;            /* would run past the load window */
            }
            if (ata_read_sector(lba + i, dst + done) != 0) {
                return done;
            }
            done += SECTOR_SIZE;
        }
        clus = fat_next(fi, clus);
    }
    return done > size ? size : done;
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
    struct fat_info fi;
    u32 sp, pc, fs_start, loaded = 0;
    int i;

    uart_init();
    uart_puts("\nSage040 boot ROM\n");

    /* The payload must not land on top of the boot ROM. */
    if (LOAD_LIMIT > BOOTROM_BASE) {
        uart_puts("BOOT: payload window overlaps the boot ROM\n");
        return 0;
    }

    fs_start = partition_start();

    /* ---- preferred path: KERNEL.ROM out of the filesystem ---- */
    if (fs_start) {
        uart_puts("partition 1 at LBA ");
        uart_putdec(fs_start);
        uart_puts(", type 0x");
        uart_puthex8(mbr[MBR_PART0 + MBR_PART_TYPE]);
        uart_puts("\n");

        if (fat_mount(fs_start, &fi) == 0) {
            u32 size = 0;
            u16 clus = fat_find(&fi, "KERNEL  ROM", &size);

            if (clus) {
                uart_puts("KERNEL.ROM  ");
                uart_putdec(size);
                uart_puts(" bytes, first cluster ");
                uart_putdec(clus);
                uart_puts("\n");

                loaded = fat_load(&fi, clus, size, load);
                if (loaded < size) {
                    uart_puts("BOOT: short read (");
                    uart_putdec(loaded);
                    uart_puts(" of ");
                    uart_putdec(size);
                    uart_puts(" bytes)\n");
                    return 0;
                }
            } else {
                uart_puts("no KERNEL.ROM in the root directory\n");
            }
        } else {
            uart_puts("partition is not a FAT16 filesystem"
                      " this loader understands\n");
        }
    } else {
        uart_puts("no partition table\n");
    }

    /* ---- fallback: a raw image in the boot gap ---- */
    if (!loaded) {
        uart_puts("falling back to a raw image at LBA ");
        uart_putdec(KERNEL_LBA);
        uart_puts("\n");

        if (fs_start && KERNEL_LBA + (u32)BOOT_SECTORS > fs_start) {
            uart_puts("BOOT: the raw region would run into the filesystem\n");
            return 0;
        }

        for (i = 0; i < BOOT_SECTORS; i++) {
            if (ata_read_sector(KERNEL_LBA + (u32)i,
                                load + (u32)i * SECTOR_SIZE) != 0) {
                uart_puts("BOOT: read failed at sector ");
                uart_putdec(KERNEL_LBA + (u32)i);
                uart_puts("\n");
                return 0;
            }
        }
        loaded = (u32)BOOT_SECTORS * SECTOR_SIZE;
    }

    /* First two longs of the image are the 68000 reset vectors. */
    sp = image[0];
    pc = image[1];

    uart_puts("image SSP = 0x"); uart_puthex32(sp);
    uart_puts("  PC = 0x");      uart_puthex32(pc);
    uart_puts("\n");

    /*
     * Sanity checks. Blank media reads back as zeros, which would
     * otherwise send us to address 0 with a null stack and no clue why.
     */
    if (pc < LOAD_ADDR + 8 || pc >= LOAD_ADDR + loaded) {
        uart_puts("BOOT: entry point outside the loaded image\n");
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
