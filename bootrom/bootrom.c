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
 * Disk layout. The disk is an ordinary PC disk carrying an ordinary
 * ext2 filesystem, so the host can read and write it with e2fsprogs —
 * mke2fs, e2fsck, debugfs — on the plain image file:
 *
 *     LBA 0             MBR partition table
 *     LBA 64            optional raw kernel, in the boot gap
 *     LBA 2048          partition 1, type 0x83, ext2
 *
 * The normal path is to find KERNEL.ROM in the root directory of that
 * filesystem and load it. That is the point of using a real format:
 * replacing the kernel is a file copy, not a dd to a sector number.
 *
 * If there is no filesystem, or no KERNEL.ROM in it, the loader falls
 * back to reading a raw image from LBA 64 — the gap between the
 * partition table and the first partition, which is the classic place to
 * put a boot image and is nearly a megabyte here. That keeps a
 * filesystem-less disk bootable.
 *
 * Only what is needed to find one file in the root directory is
 * implemented: the superblock, group 0's descriptor, one inode, and a
 * block map of direct and singly indirect blocks — which reaches 4 MB at
 * the 4 KB block size the disk is made with, so a kernel never needs
 * double indirection here. Everything in ext2 is little-endian and this
 * machine is not, so every multi-byte field goes through le16()/le32().
 *
 * FAT16 is NOT read here. The kernel still mounts a FAT volume, so a
 * disk from a machine that has never heard of this one is still
 * readable once the kernel is up; what the ROM has to find is this
 * machine's own kernel, and that lives on this machine's own disk.
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
/* ext2, read only, enough to find one file in the root directory    */
/* ---------------------------------------------------------------- */

#define EXT2_SUPER_OFF      1024        /* bytes into the filesystem   */
#define EXT2_SUPER_MAGIC    0xef53
#define EXT2_ROOT_INO       2
#define EXT2_MAX_BLOCK      4096

/* Superblock fields, from the start of the superblock. */
#define SB_LOG_BLOCK_SIZE   24
#define SB_BLOCKS_PER_GROUP 32
#define SB_INODES_PER_GROUP 40
#define SB_MAGIC            56
#define SB_REV_LEVEL        76
#define SB_INODE_SIZE       88
#define SB_FIRST_DATA_BLOCK 20

/* Group descriptor. */
#define GD_INODE_TABLE       8

/* Inode. */
#define IN_SIZE              4
#define IN_BLOCK            40
#define EXT2_NDIR_BLOCKS    12
#define EXT2_IND_BLOCK      12

/* Directory entry. */
#define DE_INODE             0
#define DE_REC_LEN           4
#define DE_NAME_LEN          6
#define DE_NAME              8

static u8 secbuf[SECTOR_SIZE];          /* superblock, descriptors     */
static u8 blkbuf[EXT2_MAX_BLOCK];       /* one filesystem block        */
static u8 indbuf[EXT2_MAX_BLOCK];       /* the indirect block          */

struct ext2_info {
    u32 part_lba;
    u32 block_size;
    u32 sectors_per_block;
    u32 inode_size;
    u32 inodes_per_group;
    u32 first_data_block;
    u32 gd_block;
};

static u16 le16(const u8 *p)
{
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

/* Read one filesystem block into `dst`. */
static int ext2_read_block(const struct ext2_info *fi, u32 blk, u8 *dst)
{
    u32 lba = fi->part_lba + blk * fi->sectors_per_block;
    u32 i;

    for (i = 0; i < fi->sectors_per_block; i++) {
        if (ata_read_sector(lba + i, dst + i * SECTOR_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ext2_mount(u32 part_lba, struct ext2_info *fi)
{
    u32 log_bs;

    /* The superblock is at byte 1024 of the filesystem, whatever the
     * block size is -- the third 512-byte sector. */
    if (ata_read_sector(part_lba + 2, secbuf) != 0) {
        return -1;
    }
    if (le16(&secbuf[SB_MAGIC]) != EXT2_SUPER_MAGIC) {
        return -1;
    }
    log_bs = le32(&secbuf[SB_LOG_BLOCK_SIZE]);
    if (log_bs > 2) {
        return -1;                      /* larger than 4096            */
    }
    fi->part_lba = part_lba;
    fi->block_size = 1024UL << log_bs;
    fi->sectors_per_block = fi->block_size / SECTOR_SIZE;
    fi->inodes_per_group = le32(&secbuf[SB_INODES_PER_GROUP]);
    fi->first_data_block = le32(&secbuf[SB_FIRST_DATA_BLOCK]);
    fi->inode_size = (le32(&secbuf[SB_REV_LEVEL]) == 0) ? 128 :
                     le16(&secbuf[SB_INODE_SIZE]);
    if (fi->inodes_per_group == 0 || fi->inode_size < 128) {
        return -1;
    }
    fi->gd_block = fi->first_data_block + 1;
    return 0;
}

/*
 * Copy inode `ino` into `dst` (at least inode_size bytes).  Only group 0
 * is reachable here, which is all the root directory and a kernel next
 * to it ever need.
 */
static int ext2_read_inode(const struct ext2_info *fi, u32 ino, u8 *dst)
{
    u32 g = (ino - 1) / fi->inodes_per_group;
    u32 idx = (ino - 1) % fi->inodes_per_group;
    u32 per_block = fi->block_size / 32;
    u32 table, blk, off, i;

    if (ext2_read_block(fi, fi->gd_block + g / per_block, blkbuf) != 0) {
        return -1;
    }
    table = le32(&blkbuf[(g % per_block) * 32 + GD_INODE_TABLE]);
    blk = table + (idx * fi->inode_size) / fi->block_size;
    off = (idx * fi->inode_size) % fi->block_size;
    if (ext2_read_block(fi, blk, blkbuf) != 0) {
        return -1;
    }
    for (i = 0; i < fi->inode_size; i++) {
        dst[i] = blkbuf[off + i];
    }
    return 0;
}

/*
 * The disk block behind file block `n` of an inode: twelve direct
 * entries, then one indirect block.  Anything past that returns 0, which
 * a 4 MB ceiling at a 4 KB block size puts well out of a kernel's way.
 */
static u32 ext2_bmap(const struct ext2_info *fi, const u8 *ino, u32 n)
{
    u32 ind;

    if (n < EXT2_NDIR_BLOCKS) {
        return le32(&ino[IN_BLOCK + 4 * n]);
    }
    n -= EXT2_NDIR_BLOCKS;
    if (n >= fi->block_size / 4) {
        return 0;
    }
    ind = le32(&ino[IN_BLOCK + 4 * EXT2_IND_BLOCK]);
    if (ind == 0) {
        return 0;
    }
    if (ext2_read_block(fi, ind, indbuf) != 0) {
        return 0;
    }
    return le32(&indbuf[4 * n]);
}

static int name_matches(const u8 *ent, const char *want)
{
    u32 n = ent[DE_NAME_LEN];
    u32 i;

    for (i = 0; i < n; i++) {
        if (want[i] == '\0' || ent[DE_NAME + i] != (u8)want[i]) {
            return 0;
        }
    }
    return want[n] == '\0';
}

/*
 * Find `name` in the root directory. Returns its inode number, or 0.
 *
 * Entries are walked by rec_len, never by a fixed step: rec_len is the
 * whole slot and a deleted entry is absorbed into the one before it, so
 * anything else desynchronises on the first directory that has ever had
 * a file removed from it.
 */
static u32 ext2_find(const struct ext2_info *fi, const char *name, u32 *size)
{
    u8 rootino[256];
    u32 dsize, off;

    if (fi->inode_size > sizeof(rootino)) {
        return 0;
    }
    if (ext2_read_inode(fi, EXT2_ROOT_INO, rootino) != 0) {
        return 0;
    }
    dsize = le32(&rootino[IN_SIZE]);

    for (off = 0; off < dsize; off += fi->block_size) {
        u32 blk = ext2_bmap(fi, rootino, off / fi->block_size);
        u32 p = 0;

        if (blk == 0) {
            continue;
        }
        if (ext2_read_block(fi, blk, blkbuf) != 0) {
            return 0;
        }
        while (p + DE_NAME <= fi->block_size) {
            u32 rec = le16(&blkbuf[p + DE_REC_LEN]);
            u32 ino = le32(&blkbuf[p + DE_INODE]);

            if (rec < DE_NAME || p + rec > fi->block_size) {
                break;
            }
            if (ino != 0 && name_matches(&blkbuf[p], name)) {
                u8 fino[256];

                if (ext2_read_inode(fi, ino, fino) != 0) {
                    return 0;
                }
                *size = le32(&fino[IN_SIZE]);
                return ino;
            }
            p += rec;
        }
    }
    return 0;
}

/* Copy the file to dst, block by block. Returns bytes read. */
static u32 ext2_load(const struct ext2_info *fi, u32 ino, u32 size, u8 *dst)
{
    u8 fino[256];
    u32 done = 0, n = 0;

    if (fi->inode_size > sizeof(fino)) {
        return 0;
    }
    if (ext2_read_inode(fi, ino, fino) != 0) {
        return 0;
    }
    while (done < size) {
        u32 blk = ext2_bmap(fi, fino, n);
        u32 i;

        if (done + fi->block_size > LOAD_LIMIT - LOAD_ADDR) {
            return done;                /* would run past the window   */
        }
        if (blk == 0) {
            return done;                /* a hole: a kernel has none   */
        }
        /* Straight into place, not through blkbuf -- which ext2_bmap
         * uses for the indirect block and would otherwise overwrite. */
        for (i = 0; i < fi->sectors_per_block; i++) {
            if (ata_read_sector(fi->part_lba + blk * fi->sectors_per_block + i,
                                dst + done + i * SECTOR_SIZE) != 0) {
                return done;
            }
        }
        done += fi->block_size;
        n++;
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
    struct ext2_info fi;
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

        if (ext2_mount(fs_start, &fi) == 0) {
            u32 size = 0;
            u32 ino = ext2_find(&fi, "KERNEL.ROM", &size);

            if (ino) {
                uart_puts("KERNEL.ROM  ");
                uart_putdec(size);
                uart_puts(" bytes, inode ");
                uart_putdec(ino);
                uart_puts("\n");

                loaded = ext2_load(&fi, ino, size, load);
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
            uart_puts("partition is not an ext2 filesystem"
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
