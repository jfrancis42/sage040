/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ext2.c - the second extended filesystem, read and write.
 *
 * Registered with the VFS as the filesystem type "ext2" and mounted on
 * a struct blockdev, exactly as fs/fat16.c is: nothing above this file
 * names ext2, and this file names no disk controller.
 *
 * Reference: the ext2 on-disk layout as e2fsprogs writes it and as
 * Linux's fs/ext2 reads it.  The disk is a genuine ext2 disk and stays
 * that way -- mke2fs, e2fsck, debugfs and dumpe2fs all work on an image
 * this kernel wrote, without root and without a loop device, which is
 * what lets a test verify the kernel's writes with somebody else's
 * code.
 *
 * WHAT THIS BUYS OVER FAT16, and why the machine's disk is ext2 now:
 * real permissions and ownership, hard links, a name that is just
 * bytes, an inode number that identifies a file for its whole life,
 * sparse files, and a free-space map that does not have to be walked
 * as a chain.
 *
 * FEATURES.  A superblock carries three feature words; the rule is that
 * unknown COMPAT bits are ignorable, unknown RO_COMPAT bits mean
 * read-only, and unknown INCOMPAT bits mean do not touch it at all.
 * This driver has no read-only mount, so it refuses anything it does
 * not implement:
 *
 *   INCOMPAT   FILETYPE    -- honoured: a directory entry carries the
 *                             type, so readdir needs no inode read
 *   RO_COMPAT  SPARSE_SUPER-- honoured: only some groups keep backups
 *              LARGE_FILE  -- accepted: sizes here never exceed 32 bits
 *   COMPAT     anything    -- ignored, EXCEPT DIR_INDEX (see below)
 *
 * DIR_INDEX is refused rather than ignored.  A hashed directory is
 * readable by a driver that knows nothing about it -- the interior
 * nodes are shaped like empty entries on purpose -- but writing into
 * one without maintaining the hash tree leaves an index that no longer
 * finds the names underneath it.  Making the volume with
 * `mke2fs -O ^dir_index` is what disk.mk does.
 *
 * BYTE ORDER.  Every multi-byte field on an ext2 disk is little-endian
 * and this CPU is not, so nothing is read by casting a pointer.  It all
 * goes through le16()/le32() and their write counterparts, which work a
 * byte at a time and so are indifferent to alignment as well.
 *
 * TIMESTAMPS reach 2106, not 2038.  This kernel's time_t is an unsigned
 * 32-bit count and ext2's i_mtime is a signed one, so a date past
 * 2038-01-19 is stored as the same 32 bits with the inode's spare
 * "extra" word set to epoch 1 -- which is how ext4 spells it, and what
 * makes Linux and e2fsprogs read back the date this kernel meant.  It
 * needs the extra word to exist, so volumes are made with 256-byte
 * inodes.  On a 128-byte-inode volume the driver still works and dates
 * past 2038 simply come back as themselves here and as 1901 elsewhere.
 *
 * i_blocks IS IN 512-BYTE UNITS, always, whatever the block size is.
 * That is not a quirk of this driver; it is the format.
 */
#include "timer.h"
#include "vfs.h"
#include "dev.h"
#include "time.h"
#include "errno.h"
#include "string.h"

/* ---------------------------------------------------------------- */
/* Little-endian accessors                                           */
/* ---------------------------------------------------------------- */

static u16 le16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void put_le16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
}

static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
    p[2] = (u8)((v >> 16) & 0xff);
    p[3] = (u8)((v >> 24) & 0xff);
}

/* ---------------------------------------------------------------- */
/* On-disk layout                                                    */
/* ---------------------------------------------------------------- */

#define SECTOR_SIZE         512

#define EXT2_SUPER_OFF      1024        /* always, whatever the block size */
#define EXT2_SUPER_MAGIC    0xef53
#define EXT2_ROOT_INO       2
#define EXT2_GOOD_OLD_FIRST_INO   11
#define EXT2_GOOD_OLD_INODE_SIZE  128

#define EXT2_MAX_BLOCK      4096        /* the largest this driver takes  */
#define EXT2_MIN_BLOCK      1024

/* Superblock field offsets, from the start of the superblock. */
#define SB_INODES_COUNT      0
#define SB_BLOCKS_COUNT      4
#define SB_R_BLOCKS_COUNT    8
#define SB_FREE_BLOCKS      12
#define SB_FREE_INODES      16
#define SB_FIRST_DATA_BLOCK 20
#define SB_LOG_BLOCK_SIZE   24
#define SB_LOG_FRAG_SIZE    28
#define SB_BLOCKS_PER_GROUP 32
#define SB_FRAGS_PER_GROUP  36
#define SB_INODES_PER_GROUP 40
#define SB_MTIME            44
#define SB_WTIME            48
#define SB_MNT_COUNT        52
#define SB_MAX_MNT_COUNT    54
#define SB_MAGIC            56
#define SB_STATE            58
#define SB_ERRORS           60
#define SB_LASTCHECK        64
#define SB_CHECKINTERVAL    68
#define SB_CREATOR_OS       72
#define SB_REV_LEVEL        76
#define SB_FIRST_INO        84
#define SB_INODE_SIZE       88
#define SB_BLOCK_GROUP_NR   90
#define SB_FEATURE_COMPAT   92
#define SB_FEATURE_INCOMPAT 96
#define SB_FEATURE_RO_COMPAT 100
#define SB_UUID            104
#define SB_VOLUME_NAME     120          /* 16 bytes                       */
#define SB_LAST_ORPHAN     232          /* head of the orphan inode list  */

#define EXT2_VALID_FS       0x0001      /* s_state: unmounted cleanly     */
#define EXT2_ERROR_FS       0x0002

#define FEAT_COMPAT_DIR_INDEX    0x0020
#define FEAT_INCOMPAT_FILETYPE   0x0002
#define FEAT_RO_SPARSE_SUPER     0x0001
#define FEAT_RO_LARGE_FILE       0x0002

/* Everything this driver knows how to honour.  Anything outside these
 * masks stops the mount rather than being guessed at. */
#define FEAT_INCOMPAT_OK    (FEAT_INCOMPAT_FILETYPE)
#define FEAT_RO_OK          (FEAT_RO_SPARSE_SUPER | FEAT_RO_LARGE_FILE)

/* Group descriptor, 32 bytes. */
#define GD_BLOCK_BITMAP      0
#define GD_INODE_BITMAP      4
#define GD_INODE_TABLE       8
#define GD_FREE_BLOCKS      12
#define GD_FREE_INODES      14
#define GD_USED_DIRS        16
#define GD_SIZE             32

/* Inode, first 128 bytes. */
#define IN_MODE              0
#define IN_UID               2
#define IN_SIZE              4
#define IN_ATIME             8
#define IN_CTIME            12
#define IN_MTIME            16
#define IN_DTIME            20
#define IN_GID              24
#define IN_LINKS_COUNT      26
#define IN_BLOCKS           28
#define IN_FLAGS            32
#define IN_BLOCK            40          /* 15 u32: 12 direct, 3 indirect  */
#define IN_GENERATION      100
#define IN_FILE_ACL        104
#define IN_DIR_ACL         108          /* the high 32 bits of a file size */
/* Past 128, present only when the inode is bigger than that. */
#define IN_EXTRA_ISIZE     128
#define IN_CTIME_EXTRA     132
#define IN_MTIME_EXTRA     136
#define IN_ATIME_EXTRA     140

#define EXT2_NDIR_BLOCKS    12
#define EXT2_IND_BLOCK      12
#define EXT2_DIND_BLOCK     13
#define EXT2_TIND_BLOCK     14
#define EXT2_N_BLOCKS       15

/* Directory entry: inode, rec_len, name_len, file_type, then the name. */
#define DE_INODE             0
#define DE_REC_LEN           4
#define DE_NAME_LEN          6
#define DE_FILE_TYPE         7
#define DE_NAME              8
#define DE_MIN_SIZE          8

#define EXT2_FT_UNKNOWN      0
#define EXT2_FT_REG_FILE     1
#define EXT2_FT_DIR          2
#define EXT2_FT_SYMLINK      7

/* ---------------------------------------------------------------- */
/* Mount state                                                       */
/* ---------------------------------------------------------------- */

static struct blockdev *dev;
static int  mounted;
static u32  part_lba;           /* LBA the filesystem starts at         */

static u32  block_size;
static u32  sectors_per_block;
static u32  inodes_count;
static u32  blocks_count;
static u32  r_blocks_count;
static u32  free_blocks;
static u32  free_inodes;
static u32  first_data_block;
static u32  blocks_per_group;
static u32  inodes_per_group;
static u32  inode_size;
static u32  first_ino;
static u32  group_count;
static u32  gd_first_block;     /* block holding the descriptor table   */
static u32  addrs_per_block;    /* block_size / 4                       */
static u32  alloc_goal;         /* where the last allocation landed     */
static int  sb_dirty;
/*
 * Whether the volume was in use when it was mounted -- read from the
 * superblock BEFORE mounting marks it in use, which is the only moment
 * the answer exists. Read afterwards it is always "dirty", and the
 * boot-time check then runs on every boot and reports every previous
 * run as unclean.
 */
static int  was_unclean;
static char volume_label[17];

/* ---------------------------------------------------------------- */
/* Block cache                                                       */
/*                                                                   */
/* Write-back, least-recently-used.  Sixteen blocks is 64 KB of a      */
/* 64 MB machine and is enough to keep a bitmap, an inode table block, */
/* an indirect block and a directory block all resident through one    */
/* operation, which is what stops a create from re-reading each of      */
/* them several times.                                                 */
/* ---------------------------------------------------------------- */

#define NBUF    16

struct bbuf {
    u8  data[EXT2_MAX_BLOCK];
    u32 blk;
    u32 stamp;                  /* for LRU                              */
    u8  valid;
    u8  dirty;
};

static struct bbuf bcache[NBUF];
static u32 bclock;

/* The superblock lives at byte 1024, which is inside block 0 when the
 * block size is 2048 or 4096 and is block 1 when it is 1024.  It is read
 * and written through its own buffer rather than the cache, so that
 * nothing can evict it half-written. */
static u8 sbuf[EXT2_SUPER_OFF + 1024];

static int bwrite_raw(u32 blk, const u8 *data)
{
    if (dev->write(dev, part_lba + blk * sectors_per_block,
                   sectors_per_block, data) != 0) {
        return -EIO;
    }
    return 0;
}

static int bread_raw(u32 blk, u8 *data)
{
    if (dev->read(dev, part_lba + blk * sectors_per_block,
                  sectors_per_block, data) != 0) {
        return -EIO;
    }
    return 0;
}

static int bflush(struct bbuf *b)
{
    int err;

    if (!b->valid || !b->dirty) {
        return 0;
    }
    err = bwrite_raw(b->blk, b->data);
    if (err != 0) {
        return err;
    }
    b->dirty = 0;
    return 0;
}

/*
 * The buffer holding block `blk`, reading it if it is not already here.
 * Returns null on an I/O error, which every caller has to check: a
 * silently zero buffer would be read as a hole.
 */
static struct bbuf *bget(u32 blk)
{
    struct bbuf *victim = &bcache[0];
    int i;

    if (blk >= blocks_count) {
        return 0;
    }
    for (i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].blk == blk) {
            bcache[i].stamp = ++bclock;
            return &bcache[i];
        }
    }
    for (i = 0; i < NBUF; i++) {
        if (!bcache[i].valid) {
            victim = &bcache[i];
            break;
        }
        if (bcache[i].stamp < victim->stamp) {
            victim = &bcache[i];
        }
    }
    if (bflush(victim) != 0) {
        return 0;
    }
    if (bread_raw(blk, victim->data) != 0) {
        victim->valid = 0;
        return 0;
    }
    victim->blk = blk;
    victim->valid = 1;
    victim->dirty = 0;
    victim->stamp = ++bclock;
    return victim;
}

/*
 * A buffer for a block that is about to be overwritten completely --
 * a freshly allocated one.  Skips the read, which is the whole point.
 */
static struct bbuf *bget_zero(u32 blk)
{
    struct bbuf *victim = &bcache[0];
    int i;

    if (blk >= blocks_count) {
        return 0;
    }
    for (i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].blk == blk) {
            victim = &bcache[i];
            goto found;
        }
    }
    for (i = 0; i < NBUF; i++) {
        if (!bcache[i].valid) {
            victim = &bcache[i];
            break;
        }
        if (bcache[i].stamp < victim->stamp) {
            victim = &bcache[i];
        }
    }
    if (bflush(victim) != 0) {
        return 0;
    }
found:
    memset(victim->data, 0, block_size);
    victim->blk = blk;
    victim->valid = 1;
    victim->dirty = 1;
    victim->stamp = ++bclock;
    return victim;
}

static void bdirty(struct bbuf *b)
{
    b->dirty = 1;
}

/* Drop a block from the cache without writing it: it has just been
 * freed, and writing it back would be writing over whoever got it
 * next. */
static void bforget(u32 blk)
{
    int i;

    for (i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].blk == blk) {
            bcache[i].valid = 0;
            bcache[i].dirty = 0;
        }
    }
}

static void bcache_reset(void)
{
    int i;

    for (i = 0; i < NBUF; i++) {
        bcache[i].valid = 0;
        bcache[i].dirty = 0;
    }
    bclock = 0;
}

static int bcache_flush_all(void)
{
    int i, first = 0, err;

    for (i = 0; i < NBUF; i++) {
        err = bflush(&bcache[i]);
        if (err != 0 && first == 0) {
            first = err;
        }
    }
    return first;
}

/* ---------------------------------------------------------------- */
/* The superblock                                                    */
/* ---------------------------------------------------------------- */

/*
 * The superblock's own sectors, read and written whole.  It always
 * starts at byte 1024 of the filesystem, so it is the third and fourth
 * 512-byte sector whatever the block size is -- which is why this does
 * not go through the block cache, whose unit is a block.
 */
static int sb_read(void)
{
    if (dev->read(dev, part_lba + 2, 2, sbuf + EXT2_SUPER_OFF) != 0) {
        return -EIO;
    }
    return 0;
}

static int sb_write(void)
{
    if (!sb_dirty) {
        return 0;
    }
    if (dev->write(dev, part_lba + 2, 2, sbuf + EXT2_SUPER_OFF) != 0) {
        return -EIO;
    }
    sb_dirty = 0;
    return 0;
}

static u8 *sbp(u32 off)
{
    return sbuf + EXT2_SUPER_OFF + off;
}

static void sb_set_free_counts(void)
{
    put_le32(sbp(SB_FREE_BLOCKS), free_blocks);
    put_le32(sbp(SB_FREE_INODES), free_inodes);
    sb_dirty = 1;
}

/* ---------------------------------------------------------------- */
/* Group descriptors                                                 */
/* ---------------------------------------------------------------- */

/*
 * Point at group `g`'s descriptor inside a cached block.  The pointer is
 * only good until the next bget(), which is why every caller reads or
 * writes through it immediately and does not hold it.
 */
static u8 *gd_get(u32 g, struct bbuf **bp)
{
    u32 per_block = block_size / GD_SIZE;
    struct bbuf *b;

    if (g >= group_count) {
        return 0;
    }
    b = bget(gd_first_block + g / per_block);
    if (!b) {
        return 0;
    }
    *bp = b;
    return b->data + (g % per_block) * GD_SIZE;
}

static int gd_add_free_blocks(u32 g, int delta)
{
    struct bbuf *b;
    u8 *d = gd_get(g, &b);
    u16 v;

    if (!d) {
        return -EIO;
    }
    v = le16(d + GD_FREE_BLOCKS);
    put_le16(d + GD_FREE_BLOCKS, (u16)(v + delta));
    bdirty(b);
    return 0;
}

static int gd_add_free_inodes(u32 g, int delta, int dir_delta)
{
    struct bbuf *b;
    u8 *d = gd_get(g, &b);
    u16 v;

    if (!d) {
        return -EIO;
    }
    v = le16(d + GD_FREE_INODES);
    put_le16(d + GD_FREE_INODES, (u16)(v + delta));
    if (dir_delta != 0) {
        v = le16(d + GD_USED_DIRS);
        put_le16(d + GD_USED_DIRS, (u16)(v + dir_delta));
    }
    bdirty(b);
    return 0;
}

static u32 gd_field(u32 g, u32 off)
{
    struct bbuf *b;
    u8 *d = gd_get(g, &b);

    return d ? le32(d + off) : 0;
}

/* ---------------------------------------------------------------- */
/* Bitmaps                                                           */
/*                                                                   */
/* One block each, per group, bit 0 of byte 0 first.  A set bit is in   */
/* use.  Both allocators take the first free bit at or after a goal and */
/* wrap, so that a file's blocks land near each other and near its      */
/* inode without any more bookkeeping than one remembered number.       */
/* ---------------------------------------------------------------- */

static int bitmap_test(const u8 *map, u32 bit)
{
    return (map[bit >> 3] >> (bit & 7)) & 1;
}

static void bitmap_set(u8 *map, u32 bit)
{
    map[bit >> 3] |= (u8)(1u << (bit & 7));
}

static void bitmap_clear(u8 *map, u32 bit)
{
    map[bit >> 3] &= (u8)~(1u << (bit & 7));
}

/* The first zero bit in [start, limit), or limit. */
static u32 bitmap_first_free(const u8 *map, u32 start, u32 limit)
{
    u32 i = start;

    /* Whole bytes that are entirely in use are the common case on a
     * full-ish group, so skip them eight at a time. */
    while (i < limit) {
        if ((i & 7) == 0) {
            while (i + 8 <= limit && map[i >> 3] == 0xff) {
                i += 8;
            }
            if (i >= limit) {
                break;
            }
        }
        if (!bitmap_test(map, i)) {
            return i;
        }
        i++;
    }
    return limit;
}

/* How many blocks group `g` actually has: the last group is short. */
static u32 group_blocks(u32 g)
{
    u32 base = first_data_block + g * blocks_per_group;

    if (base + blocks_per_group > blocks_count) {
        return blocks_count - base;
    }
    return blocks_per_group;
}

/*
 * Allocate one block, preferring one near `goal`.  Returns 0 on failure,
 * which is a safe sentinel: block 0 is never allocatable.
 */
static u32 block_alloc(u32 goal)
{
    u32 start_g, g, i, n, bit, limit, blk;
    struct bbuf *b;
    u8 *map;

    if (free_blocks == 0) {
        return 0;
    }
    if (goal < first_data_block || goal >= blocks_count) {
        goal = first_data_block;
    }
    start_g = (goal - first_data_block) / blocks_per_group;

    for (n = 0; n < group_count; n++) {
        g = (start_g + n) % group_count;
        if (gd_field(g, GD_BLOCK_BITMAP) == 0) {
            continue;
        }
        b = bget(gd_field(g, GD_BLOCK_BITMAP));
        if (!b) {
            return 0;
        }
        map = b->data;
        limit = group_blocks(g);
        i = (n == 0) ? (goal - first_data_block) % blocks_per_group : 0;
        bit = bitmap_first_free(map, i, limit);
        if (bit == limit && i != 0) {
            bit = bitmap_first_free(map, 0, i);
            if (bit >= i) {
                continue;
            }
        }
        if (bit >= limit) {
            continue;
        }
        bitmap_set(map, bit);
        bdirty(b);
        blk = first_data_block + g * blocks_per_group + bit;
        gd_add_free_blocks(g, -1);
        free_blocks--;
        sb_set_free_counts();
        alloc_goal = blk;
        return blk;
    }
    return 0;
}

static void block_free(u32 blk)
{
    u32 g, bit;
    struct bbuf *b;

    if (blk < first_data_block || blk >= blocks_count) {
        return;
    }
    bforget(blk);
    g = (blk - first_data_block) / blocks_per_group;
    bit = (blk - first_data_block) % blocks_per_group;
    b = bget(gd_field(g, GD_BLOCK_BITMAP));
    if (!b) {
        return;
    }
    if (!bitmap_test(b->data, bit)) {
        return;                 /* already free: do not double-count */
    }
    bitmap_clear(b->data, bit);
    bdirty(b);
    gd_add_free_blocks(g, 1);
    free_blocks++;
    sb_set_free_counts();
}

/*
 * Allocate an inode.  A directory goes in the group with the most free
 * blocks, so that a tree spreads out and its files have room beside it;
 * everything else goes in its parent's group, so that a file is near the
 * directory that names it.
 */
static u32 inode_alloc(int is_dir, u32 parent_group)
{
    u32 start_g = parent_group, g, n, bit, limit, best = 0, bestfree = 0;
    struct bbuf *b;

    if (free_inodes == 0) {
        return 0;
    }
    if (is_dir) {
        for (g = 0; g < group_count; g++) {
            struct bbuf *gb;
            u8 *d = gd_get(g, &gb);
            u32 f;

            if (!d) {
                return 0;
            }
            if (le16(d + GD_FREE_INODES) == 0) {
                continue;
            }
            f = le16(d + GD_FREE_BLOCKS);
            if (best == 0 || f > bestfree) {
                best = g + 1;
                bestfree = f;
            }
        }
        if (best != 0) {
            start_g = best - 1;
        }
    }
    if (start_g >= group_count) {
        start_g = 0;
    }

    for (n = 0; n < group_count; n++) {
        g = (start_g + n) % group_count;
        if (gd_field(g, GD_INODE_BITMAP) == 0) {
            continue;
        }
        b = bget(gd_field(g, GD_INODE_BITMAP));
        if (!b) {
            return 0;
        }
        limit = inodes_per_group;
        bit = bitmap_first_free(b->data, 0, limit);
        if (bit >= limit) {
            continue;
        }
        {
            u32 ino = g * inodes_per_group + bit + 1;

            if (ino < first_ino && ino != EXT2_ROOT_INO) {
                /* A reserved inode number; look past it. */
                u32 skip = (first_ino - 1) % inodes_per_group;

                bit = bitmap_first_free(b->data, skip, limit);
                if (bit >= limit) {
                    continue;
                }
                ino = g * inodes_per_group + bit + 1;
            }
            bitmap_set(b->data, bit);
            bdirty(b);
            gd_add_free_inodes(g, -1, is_dir ? 1 : 0);
            free_inodes--;
            sb_set_free_counts();
            return ino;
        }
    }
    return 0;
}

static void inode_free(u32 ino, int was_dir)
{
    u32 g, bit;
    struct bbuf *b;

    if (ino < first_ino && ino != EXT2_ROOT_INO) {
        return;
    }
    if (ino == 0 || ino > inodes_count) {
        return;
    }
    g = (ino - 1) / inodes_per_group;
    bit = (ino - 1) % inodes_per_group;
    b = bget(gd_field(g, GD_INODE_BITMAP));
    if (!b) {
        return;
    }
    if (!bitmap_test(b->data, bit)) {
        return;
    }
    bitmap_clear(b->data, bit);
    bdirty(b);
    gd_add_free_inodes(g, 1, was_dir ? -1 : 0);
    free_inodes++;
    sb_set_free_counts();
}

/* ---------------------------------------------------------------- */
/* Inodes                                                            */
/* ---------------------------------------------------------------- */

struct einode {
    u32 ino;
    u16 mode;
    u16 uid;
    u16 gid;
    u16 links;
    u32 size;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 dtime;
    u32 blocks;                 /* 512-byte units, the format's own    */
    u32 flags;
    u32 block[EXT2_N_BLOCKS];
};

/*
 * Where inode `ino` lives.  Inode numbers start at 1, so the arithmetic
 * is one-based; getting that wrong reads the inode before the one asked
 * for, which is a real file and so does not look like an error.
 */
static int inode_where(u32 ino, u32 *blk, u32 *off)
{
    u32 g, idx, table;

    if (ino == 0 || ino > inodes_count) {
        return -EINVAL;
    }
    g = (ino - 1) / inodes_per_group;
    idx = (ino - 1) % inodes_per_group;
    table = gd_field(g, GD_INODE_TABLE);
    if (table == 0) {
        return -EIO;
    }
    *blk = table + (idx * inode_size) / block_size;
    *off = (idx * inode_size) % block_size;
    return 0;
}

/*
 * A time out of an inode.  Values past 2038 are stored as a negative
 * signed second with the extra word's epoch bits set to 1; read as an
 * unsigned 32-bit count both cases come out as the same bits, so the
 * only thing the extra word decides is whether a negative value means
 * "after 2038" (keep it) or "before 1970" (which this kernel cannot
 * represent, so clamp).
 */
static u32 itime_get(const u8 *raw, u32 off, u32 extra_off)
{
    u32 v = le32(raw + off);
    u32 extra = 0;

    if (inode_size > EXT2_GOOD_OLD_INODE_SIZE &&
        le16(raw + IN_EXTRA_ISIZE) >= (extra_off - IN_EXTRA_ISIZE) + 4) {
        extra = le32(raw + extra_off);
    }
    if ((v & 0x80000000u) != 0 && (extra & 3) == 0) {
        return 0;               /* before the epoch: not representable */
    }
    return v;
}

static void itime_put(u8 *raw, u32 off, u32 extra_off, u32 v)
{
    put_le32(raw + off, v);
    if (inode_size > EXT2_GOOD_OLD_INODE_SIZE &&
        le16(raw + IN_EXTRA_ISIZE) >= (extra_off - IN_EXTRA_ISIZE) + 4) {
        u32 extra = le32(raw + extra_off);

        extra &= ~3u;
        if ((v & 0x80000000u) != 0) {
            extra |= 1;         /* epoch 1: the date is after 2038      */
        }
        put_le32(raw + extra_off, extra);
    }
}

static int iread(u32 ino, struct einode *ei)
{
    struct bbuf *b;
    u32 blk, off;
    u8 *raw;
    int i, err;

    err = inode_where(ino, &blk, &off);
    if (err != 0) {
        return err;
    }
    b = bget(blk);
    if (!b) {
        return -EIO;
    }
    raw = b->data + off;
    ei->ino = ino;
    ei->mode = le16(raw + IN_MODE);
    ei->uid = le16(raw + IN_UID);
    ei->gid = le16(raw + IN_GID);
    ei->links = le16(raw + IN_LINKS_COUNT);
    ei->size = le32(raw + IN_SIZE);
    ei->atime = itime_get(raw, IN_ATIME, IN_ATIME_EXTRA);
    ei->ctime = itime_get(raw, IN_CTIME, IN_CTIME_EXTRA);
    ei->mtime = itime_get(raw, IN_MTIME, IN_MTIME_EXTRA);
    ei->dtime = le32(raw + IN_DTIME);
    ei->blocks = le32(raw + IN_BLOCKS);
    ei->flags = le32(raw + IN_FLAGS);
    for (i = 0; i < EXT2_N_BLOCKS; i++) {
        ei->block[i] = le32(raw + IN_BLOCK + 4 * i);
    }
    return 0;
}

/*
 * Write an inode back.  It re-reads the table block and updates only the
 * fields this driver models, so that a generation number, an extended
 * attribute block or anything else another implementation left there
 * survives being opened here.
 */
static int iwrite(const struct einode *ei)
{
    struct bbuf *b;
    u32 blk, off;
    u8 *raw;
    int i, err;

    err = inode_where(ei->ino, &blk, &off);
    if (err != 0) {
        return err;
    }
    b = bget(blk);
    if (!b) {
        return -EIO;
    }
    raw = b->data + off;
    put_le16(raw + IN_MODE, ei->mode);
    put_le16(raw + IN_UID, ei->uid);
    put_le16(raw + IN_GID, ei->gid);
    put_le16(raw + IN_LINKS_COUNT, ei->links);
    put_le32(raw + IN_SIZE, ei->size);
    itime_put(raw, IN_ATIME, IN_ATIME_EXTRA, ei->atime);
    itime_put(raw, IN_CTIME, IN_CTIME_EXTRA, ei->ctime);
    itime_put(raw, IN_MTIME, IN_MTIME_EXTRA, ei->mtime);
    put_le32(raw + IN_DTIME, ei->dtime);
    put_le32(raw + IN_BLOCKS, ei->blocks);
    put_le32(raw + IN_FLAGS, ei->flags);
    for (i = 0; i < EXT2_N_BLOCKS; i++) {
        put_le32(raw + IN_BLOCK + 4 * i, ei->block[i]);
    }
    bdirty(b);
    return 0;
}

/* Zero a brand new inode completely, including the parts above that are
 * not modelled: a reused inode otherwise inherits the last owner's
 * extended attribute block. */
static int iclear(u32 ino)
{
    struct bbuf *b;
    u32 blk, off;
    u8 *raw;
    int err;
    u16 extra;

    err = inode_where(ino, &blk, &off);
    if (err != 0) {
        return err;
    }
    b = bget(blk);
    if (!b) {
        return -EIO;
    }
    raw = b->data + off;
    extra = (inode_size > EXT2_GOOD_OLD_INODE_SIZE) ?
            le16(raw + IN_EXTRA_ISIZE) : 0;
    memset(raw, 0, inode_size);
    if (extra != 0) {
        put_le16(raw + IN_EXTRA_ISIZE, extra);
    }
    bdirty(b);
    return 0;
}

static u32 now_secs(void)
{
    struct timeval tv;

    clock_get(&tv);
    return (u32)tv.tv_sec;
}

/* ---------------------------------------------------------------- */
/* The block map                                                     */
/*                                                                   */
/* Twelve direct entries, then singly, doubly and triply indirect      */
/* blocks.  A zero entry is a HOLE and reads as zeroes -- it is not an  */
/* error and must not be allocated on the read path, or reading a       */
/* sparse file would fill the disk.                                     */
/* ---------------------------------------------------------------- */

/*
 * Decompose a file block number into the path through the map:
 * *levels is 0 for a direct block, 1..3 for indirect, and idx[] holds
 * the index to take at each step starting from the inode's block[].
 */
static int map_path(u32 iblk, int *levels, u32 idx[4])
{
    u32 n = addrs_per_block;

    if (iblk < EXT2_NDIR_BLOCKS) {
        *levels = 0;
        idx[0] = iblk;
        return 0;
    }
    iblk -= EXT2_NDIR_BLOCKS;
    if (iblk < n) {
        *levels = 1;
        idx[0] = EXT2_IND_BLOCK;
        idx[1] = iblk;
        return 0;
    }
    iblk -= n;
    if (iblk < n * n) {
        *levels = 2;
        idx[0] = EXT2_DIND_BLOCK;
        idx[1] = iblk / n;
        idx[2] = iblk % n;
        return 0;
    }
    iblk -= n * n;
    if (iblk / n / n < n) {
        *levels = 3;
        idx[0] = EXT2_TIND_BLOCK;
        idx[1] = iblk / (n * n);
        idx[2] = (iblk / n) % n;
        idx[3] = iblk % n;
        return 0;
    }
    return -EFBIG;
}

/*
 * The disk block behind file block `iblk`, allocating the path to it
 * when `alloc` is set.  *out is 0 for a hole when alloc is clear.
 *
 * The inode is modified in memory when a direct or top-level indirect
 * entry is allocated; the caller writes it back.
 */
static int bmap(struct einode *ei, u32 iblk, int alloc, u32 *out)
{
    u32 idx[4], blk, next, goal;
    int levels, i, err;
    struct bbuf *b;

    *out = 0;
    err = map_path(iblk, &levels, idx);
    if (err != 0) {
        return err;
    }

    goal = alloc_goal;
    if (ei->block[0] != 0) {
        goal = ei->block[0] + iblk;
    }

    blk = ei->block[idx[0]];
    if (blk == 0) {
        if (!alloc) {
            return 0;
        }
        blk = block_alloc(goal);
        if (blk == 0) {
            return -ENOSPC;
        }
        /* Zero it, whether it is an indirect block or file data: a
         * block just taken off the free list holds whatever the last
         * file that owned it left there, and a write that fills only
         * part of it would publish the rest. */
        b = bget_zero(blk);
        if (!b) {
            block_free(blk);
            return -EIO;
        }
        ei->block[idx[0]] = blk;
        ei->blocks += block_size / 512;
    }

    for (i = 1; i <= levels; i++) {
        b = bget(blk);
        if (!b) {
            return -EIO;
        }
        next = le32(b->data + 4 * idx[i]);
        if (next == 0) {
            if (!alloc) {
                return 0;
            }
            next = block_alloc(blk);
            if (next == 0) {
                return -ENOSPC;
            }
            if (!bget_zero(next)) {
                block_free(next);
                return -EIO;
            }
            /* bget again: allocating may have evicted the buffer. */
            b = bget(blk);
            if (!b) {
                block_free(next);
                return -EIO;
            }
            put_le32(b->data + 4 * idx[i], next);
            bdirty(b);
            ei->blocks += block_size / 512;
        }
        blk = next;
    }

    *out = blk;
    return 0;
}

/*
 * Free every block of an inode from file block `from` onward, including
 * any indirect block that becomes empty.  This is the whole of truncate
 * except for the size, and is what unlink uses to empty a file.
 *
 * It walks the levels recursively over a fixed depth of three, so the
 * recursion cannot run away.
 */
static int free_branch(u32 blk, int level, u32 first, struct einode *ei)
{
    struct bbuf *b;
    u32 i, entry, span = 1;
    int l, all_gone = 1;

    if (blk == 0) {
        return 1;
    }
    if (level == 0) {
        block_free(blk);
        ei->blocks -= block_size / 512;
        return 1;
    }
    /* How many FILE blocks each entry of this table covers: 1 at the
     * lowest level, addrs_per_block at the next, and so on. */
    for (l = 1; l < level; l++) {
        span *= addrs_per_block;
    }

    for (i = 0; i < addrs_per_block; i++) {
        b = bget(blk);
        if (!b) {
            return 0;
        }
        entry = le32(b->data + 4 * i);
        if (entry == 0) {
            continue;
        }
        if ((i + 1) * span <= first) {
            all_gone = 0;       /* entirely before the cut: keep it    */
            continue;
        }
        /* At or after the cut, this whole subtree goes; straddling it,
         * only the part past `first` does. */
        if (!free_branch(entry, level - 1,
                         (i * span >= first) ? 0 : first - i * span, ei)) {
            all_gone = 0;
            continue;
        }
        b = bget(blk);
        if (!b) {
            return 0;
        }
        put_le32(b->data + 4 * i, 0);
        bdirty(b);
    }
    if (all_gone) {
        block_free(blk);
        ei->blocks -= block_size / 512;
        return 1;
    }
    return 0;
}

static int inode_truncate_blocks(struct einode *ei, u32 from)
{
    u32 n = addrs_per_block;
    u32 i;
    u32 ind_first, dind_first, tind_first;

    for (i = from; i < EXT2_NDIR_BLOCKS; i++) {
        if (ei->block[i] != 0) {
            block_free(ei->block[i]);
            ei->blocks -= block_size / 512;
            ei->block[i] = 0;
        }
    }

    ind_first  = (from < EXT2_NDIR_BLOCKS) ? 0 : from - EXT2_NDIR_BLOCKS;
    if (ind_first < n) {
        if (free_branch(ei->block[EXT2_IND_BLOCK], 1, ind_first, ei)) {
            ei->block[EXT2_IND_BLOCK] = 0;
        }
    }

    dind_first = (ind_first < n) ? 0 : ind_first - n;
    if (dind_first < n * n) {
        if (free_branch(ei->block[EXT2_DIND_BLOCK], 2, dind_first, ei)) {
            ei->block[EXT2_DIND_BLOCK] = 0;
        }
    }

    tind_first = (dind_first < n * n) ? 0 : dind_first - n * n;
    if (free_branch(ei->block[EXT2_TIND_BLOCK], 3, tind_first, ei)) {
        ei->block[EXT2_TIND_BLOCK] = 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* File data                                                         */
/* ---------------------------------------------------------------- */

static s32 inode_read(struct einode *ei, u32 pos, void *buf, u32 len)
{
    u8 *out = (u8 *)buf;
    u32 done = 0;

    if (pos >= ei->size) {
        return 0;
    }
    if (len > ei->size - pos) {
        len = ei->size - pos;
    }
    while (done < len) {
        u32 iblk = (pos + done) / block_size;
        u32 off = (pos + done) % block_size;
        u32 n = block_size - off;
        u32 blk;
        int err;

        if (n > len - done) {
            n = len - done;
        }
        err = bmap(ei, iblk, 0, &blk);
        if (err != 0) {
            return done ? (s32)done : (s32)err;
        }
        if (blk == 0) {
            memset(out + done, 0, n);   /* a hole reads as zeroes      */
        } else {
            struct bbuf *b = bget(blk);

            if (!b) {
                return done ? (s32)done : -EIO;
            }
            memcpy(out + done, b->data + off, n);
        }
        done += n;
    }
    return (s32)done;
}

static s32 inode_write(struct einode *ei, u32 pos, const void *buf, u32 len)
{
    const u8 *in = (const u8 *)buf;
    u32 done = 0;
    int dirty = 0;

    while (done < len) {
        u32 iblk = (pos + done) / block_size;
        u32 off = (pos + done) % block_size;
        u32 n = block_size - off;
        u32 blk;
        struct bbuf *b;
        int err;

        if (n > len - done) {
            n = len - done;
        }
        err = bmap(ei, iblk, 1, &blk);
        if (err != 0) {
            break;
        }
        b = bget(blk);
        if (!b) {
            err = -EIO;
            break;
        }
        memcpy(b->data + off, in + done, n);
        bdirty(b);
        done += n;
        dirty = 1;
        if (pos + done > ei->size) {
            ei->size = pos + done;
        }
    }
    if (done == 0 && len != 0) {
        return -ENOSPC;
    }
    if (dirty) {
        ei->mtime = now_secs();
        ei->ctime = ei->mtime;
    }
    return (s32)done;
}

/*
 * Set a file's length.  Growing leaves a hole -- no blocks are allocated
 * until something writes into it -- and shrinking frees what is past the
 * new end, including the partial block's tail, which has to be zeroed or
 * the old bytes reappear the moment the file grows again.
 */
static int inode_set_size(struct einode *ei, u32 len)
{
    u32 first;

    if (len < ei->size) {
        u32 off = len % block_size;

        if (off != 0) {
            u32 blk;

            if (bmap(ei, len / block_size, 0, &blk) == 0 && blk != 0) {
                struct bbuf *b = bget(blk);

                if (b) {
                    memset(b->data + off, 0, block_size - off);
                    bdirty(b);
                }
            }
        }
        first = (len + block_size - 1) / block_size;
        inode_truncate_blocks(ei, first);
    }
    ei->size = len;
    ei->mtime = now_secs();
    ei->ctime = ei->mtime;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Directories                                                       */
/*                                                                   */
/* A directory is an ordinary file whose contents are a chain of        */
/* variable-length entries.  Three rules govern every one of them:      */
/*                                                                      */
/*   rec_len is the whole slot, which may be bigger than the name in it */
/*   an entry never crosses a block boundary, so the last one in each   */
/*     block has rec_len running to the end of that block               */
/*   a deleted entry is not blanked; its space is added to the rec_len  */
/*     of the entry before it, or its inode is set to 0 when it is the  */
/*     first in the block                                               */
/*                                                                      */
/* Walking with anything other than rec_len -- by fixed steps, say --    */
/* appears to work on a fresh directory and desynchronises on the first  */
/* deletion.                                                            */
/* ---------------------------------------------------------------- */

/* The space an entry with a name this long actually needs. */
static u32 de_len(u32 namelen)
{
    return (DE_MIN_SIZE + namelen + 3) & ~3u;
}

static u32 name_len_of(const char *name)
{
    u32 n = 0;

    while (name[n] != '\0') {
        n++;
    }
    return n;
}

static int name_eq(const u8 *ent, const char *name, u32 nlen)
{
    if (ent[DE_NAME_LEN] != nlen) {
        return 0;
    }
    return memcmp(ent + DE_NAME, name, nlen) == 0;
}

/*
 * Find `name` in directory `dino`.  On success *ino is its inode and
 * *type its EXT2_FT_ code (EXT2_FT_UNKNOWN on a volume without the
 * filetype feature, which the caller then has to resolve by reading the
 * inode).
 */
static int dir_lookup(u32 dino, const char *name, u32 nlen, u32 *ino,
                      u8 *type)
{
    struct einode di;
    u32 off;
    int err;

    err = iread(dino, &di);
    if (err != 0) {
        return err;
    }
    if (!S_ISDIR(di.mode)) {
        return -ENOTDIR;
    }
    for (off = 0; off < di.size; off += block_size) {
        struct bbuf *b;
        u32 blk, p;

        err = bmap(&di, off / block_size, 0, &blk);
        if (err != 0) {
            return err;
        }
        if (blk == 0) {
            continue;
        }
        b = bget(blk);
        if (!b) {
            return -EIO;
        }
        p = 0;
        while (p + DE_MIN_SIZE <= block_size) {
            u8 *ent = b->data + p;
            u32 rec = le16(ent + DE_REC_LEN);

            if (rec < DE_MIN_SIZE || p + rec > block_size) {
                break;          /* corrupt: stop rather than run away  */
            }
            if (le32(ent + DE_INODE) != 0 && name_eq(ent, name, nlen)) {
                *ino = le32(ent + DE_INODE);
                if (type) {
                    *type = ent[DE_FILE_TYPE];
                }
                return 0;
            }
            p += rec;
        }
    }
    return -ENOENT;
}

/* Add one more block to a directory and give it a single free entry
 * spanning the whole block. */
static int dir_grow(struct einode *di, u32 *blk_out)
{
    struct bbuf *b;
    u32 blk;
    int err;

    err = bmap(di, di->size / block_size, 1, &blk);
    if (err != 0) {
        return err;
    }
    b = bget(blk);
    if (!b) {
        return -EIO;
    }
    memset(b->data, 0, block_size);
    put_le32(b->data + DE_INODE, 0);
    put_le16(b->data + DE_REC_LEN, (u16)block_size);
    b->data[DE_NAME_LEN] = 0;
    b->data[DE_FILE_TYPE] = 0;
    bdirty(b);
    di->size += block_size;
    *blk_out = blk;
    return 0;
}

/*
 * Link `name` to inode `ino` in directory `dino`.  The caller has
 * already established that the name is not there.
 */
static int dir_add(u32 dino, const char *name, u32 nlen, u32 ino, u8 type)
{
    struct einode di;
    u32 need = de_len(nlen);
    u32 off;
    int err;

    if (nlen == 0 || nlen > 255) {
        return -ENAMETOOLONG;
    }
    err = iread(dino, &di);
    if (err != 0) {
        return err;
    }

    for (off = 0; ; off += block_size) {
        struct bbuf *b;
        u32 blk, p;

        if (off >= di.size) {
            err = dir_grow(&di, &blk);
            if (err != 0) {
                return err;
            }
        } else {
            err = bmap(&di, off / block_size, 1, &blk);
            if (err != 0) {
                return err;
            }
        }
        b = bget(blk);
        if (!b) {
            return -EIO;
        }
        p = 0;
        while (p + DE_MIN_SIZE <= block_size) {
            u8 *ent = b->data + p;
            u32 rec = le16(ent + DE_REC_LEN);
            u32 used = (le32(ent + DE_INODE) == 0) ? 0 :
                       de_len(ent[DE_NAME_LEN]);

            if (rec < DE_MIN_SIZE || p + rec > block_size) {
                break;
            }
            if (rec - used >= need) {
                u8 *slot;

                if (used == 0) {
                    slot = ent;                 /* reuse the whole slot */
                } else {
                    put_le16(ent + DE_REC_LEN, (u16)used);
                    slot = ent + used;
                    put_le16(slot + DE_REC_LEN, (u16)(rec - used));
                }
                put_le32(slot + DE_INODE, ino);
                slot[DE_NAME_LEN] = (u8)nlen;
                slot[DE_FILE_TYPE] = type;
                memcpy(slot + DE_NAME, name, nlen);
                bdirty(b);
                di.mtime = now_secs();
                di.ctime = di.mtime;
                return iwrite(&di);
            }
            p += rec;
        }
    }
}

/*
 * Unlink `name` from directory `dino`, giving its space back to the
 * entry before it.  *ino_out is what it pointed at, so the caller can
 * drop the link count without looking it up again.
 */
static int dir_del(u32 dino, const char *name, u32 nlen, u32 *ino_out)
{
    struct einode di;
    u32 off;
    int err;

    err = iread(dino, &di);
    if (err != 0) {
        return err;
    }
    for (off = 0; off < di.size; off += block_size) {
        struct bbuf *b;
        u32 blk, p, prev = 0;
        int have_prev = 0;

        err = bmap(&di, off / block_size, 0, &blk);
        if (err != 0) {
            return err;
        }
        if (blk == 0) {
            continue;
        }
        b = bget(blk);
        if (!b) {
            return -EIO;
        }
        p = 0;
        while (p + DE_MIN_SIZE <= block_size) {
            u8 *ent = b->data + p;
            u32 rec = le16(ent + DE_REC_LEN);

            if (rec < DE_MIN_SIZE || p + rec > block_size) {
                break;
            }
            if (le32(ent + DE_INODE) != 0 && name_eq(ent, name, nlen)) {
                if (ino_out) {
                    *ino_out = le32(ent + DE_INODE);
                }
                if (have_prev) {
                    u8 *pe = b->data + prev;

                    put_le16(pe + DE_REC_LEN,
                             (u16)(le16(pe + DE_REC_LEN) + rec));
                } else {
                    put_le32(ent + DE_INODE, 0);
                }
                bdirty(b);
                di.mtime = now_secs();
                di.ctime = di.mtime;
                return iwrite(&di);
            }
            prev = p;
            have_prev = 1;
            p += rec;
        }
    }
    return -ENOENT;
}

/* A directory is empty when it holds nothing but "." and "..". */
static int dir_is_empty(u32 dino)
{
    struct einode di;
    u32 off;
    int err;

    err = iread(dino, &di);
    if (err != 0) {
        return err;
    }
    for (off = 0; off < di.size; off += block_size) {
        struct bbuf *b;
        u32 blk, p;

        err = bmap(&di, off / block_size, 0, &blk);
        if (err != 0 || blk == 0) {
            continue;
        }
        b = bget(blk);
        if (!b) {
            return -EIO;
        }
        p = 0;
        while (p + DE_MIN_SIZE <= block_size) {
            u8 *ent = b->data + p;
            u32 rec = le16(ent + DE_REC_LEN);
            u32 nl = ent[DE_NAME_LEN];

            if (rec < DE_MIN_SIZE || p + rec > block_size) {
                break;
            }
            if (le32(ent + DE_INODE) != 0) {
                if (!((nl == 1 && ent[DE_NAME] == '.') ||
                      (nl == 2 && ent[DE_NAME] == '.' &&
                       ent[DE_NAME + 1] == '.'))) {
                    return 0;
                }
            }
            p += rec;
        }
    }
    return 1;
}

/*
 * The nth entry of a directory, for readdir.  Index-based because that
 * is the shape of the VFS call; each step re-walks from the start,
 * which costs a directory scan per entry and is what every FAT and ext2
 * readdir that is not holding a cursor has always done.
 */
static int dir_nth(u32 dino, int index, char *name, u32 *ino, u8 *type)
{
    struct einode di;
    u32 off;
    int n = 0, err;

    err = iread(dino, &di);
    if (err != 0) {
        return err;
    }
    if (!S_ISDIR(di.mode)) {
        return -ENOTDIR;
    }
    for (off = 0; off < di.size; off += block_size) {
        struct bbuf *b;
        u32 blk, p;

        err = bmap(&di, off / block_size, 0, &blk);
        if (err != 0) {
            return err;
        }
        if (blk == 0) {
            continue;
        }
        b = bget(blk);
        if (!b) {
            return -EIO;
        }
        p = 0;
        while (p + DE_MIN_SIZE <= block_size) {
            u8 *ent = b->data + p;
            u32 rec = le16(ent + DE_REC_LEN);
            u32 nl = ent[DE_NAME_LEN];

            if (rec < DE_MIN_SIZE || p + rec > block_size) {
                break;
            }
            if (le32(ent + DE_INODE) != 0) {
                if (n == index) {
                    if (nl > NAME_MAX) {
                        nl = NAME_MAX;
                    }
                    memcpy(name, ent + DE_NAME, nl);
                    name[nl] = '\0';
                    *ino = le32(ent + DE_INODE);
                    *type = ent[DE_FILE_TYPE];
                    return 0;
                }
                n++;
            }
            p += rec;
        }
    }
    return -ENOENT;
}

/* ---------------------------------------------------------------- */
/* Paths                                                             */
/* ---------------------------------------------------------------- */

static u32 root_ino(void)
{
    u32 r = vfs_root_ino();     /* a chroot's root, or the real one   */

    return (r == 0) ? EXT2_ROOT_INO : r;
}

static u32 cwd_ino(void)
{
    u32 c = vfs_cwd_ino();

    return (c == 0) ? root_ino() : c;
}

/*
 * Resolve all of `path` except its last component, which is left in
 * out_name.  *out_dir is the directory that would contain it.
 *
 * A path ending in '/' has no last component: out_dir is the directory
 * itself, out_name is empty, and *out_last_is_dir is set.  That is the
 * same shape fs/fat16.c's path_walk() has, because the callers above
 * are the same callers.
 */
/*
 * THE TWO SHAPES OF AN ext2 SYMLINK, and both have to be read.
 *
 * A FAST symlink keeps its target in the inode itself, in the 60 bytes
 * that would otherwise be the fifteen block pointers. It has no blocks
 * at all -- i_blocks is 0 -- and that is how it is recognised. Almost
 * every symlink in practice is one of these: 60 bytes is a long path.
 *
 * A SLOW symlink is an ordinary file whose contents are the target,
 * used when the target does not fit. Reading one is reading the file.
 *
 * i_size is the length either way, and the target is NOT
 * null-terminated on disk -- writing the terminator is the reader's
 * job, and forgetting it hands the rest of the inode to the caller as
 * part of the path.
 */
#define SYMLINK_FAST_MAX  (EXT2_N_BLOCKS * 4)   /* 60 bytes */

static int read_link(u32 ino, char *out, u32 size)
{
    struct einode ei;
    int err = iread(ino, &ei);
    u32 len;

    if (err != 0) {
        return err;
    }
    if (!S_ISLNK(ei.mode)) {
        return -EINVAL;         /* what readlink(2) says for a non-link */
    }
    len = ei.size;
    if (len == 0 || len >= size) {
        return -ENAMETOOLONG;
    }
    if (ei.blocks == 0) {
        /* Fast: the target is the block-pointer array, as bytes. The
         * pointers are stored little-endian on disk and iread has
         * already byte-swapped them into u32s, so they are put back
         * the same way rather than memcpy'd -- a memcpy here would
         * reverse every group of four characters on a big-endian
         * machine, which reads as a corrupt link rather than as a
         * byte-order mistake. */
        u32 i;

        for (i = 0; i < len; i++) {
            out[i] = (char)((ei.block[i / 4] >> (8 * (i % 4))) & 0xff);
        }
    } else {
        s32 n = inode_read(&ei, 0, out, len);

        if (n < 0) {
            return (int)n;
        }
        if ((u32)n != len) {
            return -EIO;
        }
    }
    out[len] = '\0';
    return 0;
}

/*
 * How many symlinks one path resolution may follow before giving up.
 *
 * A link that points at itself, or a pair that point at each other, is
 * a loop with no end; without a limit the kernel walks it for ever
 * with the filesystem lock held, which is not a crash but a machine
 * that has stopped. Linux's limit is 40 and the number is arbitrary --
 * what matters is that there is one and that exceeding it is ELOOP
 * rather than a hang.
 */
#define SYMLINK_MAX_DEPTH 40

/*
 * THE SCRATCH FOR FOLLOWING LINKS IS STATIC, AND MUST BE.
 *
 * Two PATH_MAX buffers and a target is 3 KB; a kernel stack here is
 * four pages, 16 KB, shared with everything else the call is doing.
 * The first version of this put them on the stack of a function that
 * also RECURSED once per link -- and it did not survive a plain `rm`,
 * let alone a link: QEMU stopped with
 *
 *     qemu: fatal: DOUBLE MMU FAULT ... fault at 001afcd4
 *
 * with A7 sitting at 001afcd8, which is the stack overflowing into the
 * unmapped page below it. A fault taken while pushing a fault frame is
 * not an exception, it is the emulator aborting with no message from
 * the kernel at all.
 *
 * Static is safe because the FILESYSTEM LOCK is held for the whole of
 * every VFS operation, so exactly one task is ever inside here -- the
 * same reason fs/ can keep any other state between calls. It is not
 * safe to keep anything in them ACROSS a sleep, and nothing does: they
 * are used and finished with inside one walk.
 *
 * Two path buffers, alternating: the remainder of the old path is
 * still being read out of one while the new path is written into the
 * other.
 */
static char sl_target[PATH_MAX];
static char sl_path[2][PATH_MAX];

static int path_walk(u32 start, const char *path, u32 *out_dir,
                     char *out_name, int *out_last_is_dir)
{
    u32 d = start;
    const char *p = path;
    int depth = 0, which = 0;

    if (out_last_is_dir) {
        *out_last_is_dir = 0;
    }
    out_name[0] = '\0';

    if (*p == '/') {
        d = root_ino();
        while (*p == '/') {
            p++;
        }
    }

    for (;;) {
        char comp[NAME_MAX + 1];
        const char *slash = p;
        u32 n, ino;
        u8 type;
        int err;

        while (*slash && *slash != '/') {
            slash++;
        }
        n = (u32)(slash - p);
        if (n == 0) {
            *out_dir = d;
            if (out_last_is_dir) {
                *out_last_is_dir = 1;
            }
            return 0;
        }
        if (n > NAME_MAX) {
            return -ENAMETOOLONG;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';

        /*
         * ".." AT THE ROOT IS THE ROOT -- and "the root" means this
         * task's root, which chroot() may have moved. Unlike FAT, an
         * ext2 directory records its real parent in its own ".." entry,
         * so following that entry walks straight out of a chroot jail.
         * Clamping here is what keeps the jail a jail.
         */
        if (d == root_ino() && strcmp(comp, "..") == 0) {
            p = slash;
            while (*p == '/') {
                p++;
            }
            if (*p == '\0') {
                *out_dir = d;
                if (out_last_is_dir) {
                    *out_last_is_dir = 1;
                }
                return 0;
            }
            continue;
        }

        if (*slash == '\0') {
            /* The last component: the caller decides what it means. */
            memcpy(out_name, comp, n + 1);
            *out_dir = d;
            return 0;
        }

        /* An interior component has to exist and be a directory. */
        err = dir_lookup(d, comp, n, &ino, &type);
        if (err != 0) {
            return err;
        }
        if (type == EXT2_FT_UNKNOWN) {
            struct einode ei;

            err = iread(ino, &ei);
            if (err != 0) {
                return err;
            }
            if (S_ISLNK(ei.mode)) {
                return -ELOOP;
            }
            if (!S_ISDIR(ei.mode)) {
                return -ENOTDIR;
            }
        } else if (type == EXT2_FT_SYMLINK) {
            /*
             * FOLLOW IT. The target replaces the part of the path
             * walked so far, the rest is appended, and the whole thing
             * is walked again from the right place: from the ROOT if
             * the target is absolute, otherwise from the directory the
             * link lives in -- a relative target is relative to the
             * link, not to the working directory, and getting that
             * backwards makes `a/b -> c` resolve somewhere else
             * entirely depending on where the caller happened to be.
             */
            const char *r = slash;
            char *dst = sl_path[which];
            u32 tn, rn;

            if (++depth > SYMLINK_MAX_DEPTH) {
                return -ELOOP;
            }
            err = read_link(ino, sl_target, sizeof(sl_target));
            if (err != 0) {
                return err;
            }
            while (*r == '/') {
                r++;
            }
            tn = (u32)strlen(sl_target);
            rn = (u32)strlen(r);
            if (tn + 1 + rn + 1 > PATH_MAX) {
                return -ENAMETOOLONG;
            }
            /* `r` may point into the OTHER buffer, which is why there
             * are two: writing the new path must not overwrite the
             * remainder of the old one while it is still being read. */
            memcpy(dst, sl_target, tn);
            if (rn) {
                dst[tn] = '/';
                memcpy(dst + tn + 1, r, rn);
                dst[tn + 1 + rn] = '\0';
            } else {
                dst[tn] = '\0';
            }
            /* An absolute target restarts at the root; a relative one
             * continues from the directory the LINK is in, which is
             * where `d` already points. */
            if (sl_target[0] == '/') {
                d = root_ino();
            }
            p = dst;
            /*
             * AND PAST THE LEADING SLASHES. The top of this loop does
             * not strip them -- that happens once, before it -- so a
             * rewritten path beginning with '/' was read as an empty
             * FIRST component, which this loop takes to mean "the path
             * ended in a slash" and returns the directory. `cat
             * /dlink/one.txt` opened /adir itself and reported
             * "Is a directory".
             */
            while (*p == '/') {
                p++;
            }
            which ^= 1;
            continue;
        } else if (type != EXT2_FT_DIR) {
            return -ENOTDIR;
        }
        d = ino;
        p = slash;
        while (*p == '/') {
            p++;
        }
    }
}

/* The inode a whole path names. */
/*
 * Resolve a path WITHOUT following a symlink in its last component.
 *
 * Interior components are still followed -- `a/b/c` where `b` is a
 * link has to go through it -- because what lstat, readlink, unlink
 * and rename mean is "the last name as it stands", not "no links at
 * all".
 */
static int ext2_dir_path(u32 ino, char *out, u32 size);

static int path_resolve_nofollow(const char *path, u32 *ino)
{
    char name[NAME_MAX + 1];
    u32 d;
    int last_is_dir, err;

    err = path_walk(cwd_ino(), path, &d, name, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || name[0] == '\0') {
        *ino = d;
        return 0;
    }
    return dir_lookup(d, name, name_len_of(name), ino, 0);
}

/*
 * Resolve a path, FOLLOWING a symlink in the last component too.
 *
 * This is what open(2) and stat(2) mean: a link is a way to reach the
 * thing, and asking about `link` gives you the file. lstat, readlink,
 * unlink and rename want the other one -- path_resolve_nofollow.
 *
 * The loop is here rather than recursion because a chain of links
 * ending in a link is the ordinary case (`a -> b -> c`), and the depth
 * limit has to count the whole chain, not each step separately.
 */
static int path_resolve(const char *path, u32 *ino)
{
    /* Static for the same reason path_walk's are: three PATH_MAX
     * buffers is 3 KB, and a kernel stack is 16 KB shared with
     * everything else in the call. The filesystem lock makes it safe.
     * `rbuf` is the path being rewritten; `rtmp` builds the next one
     * because the old is still being read from the first. */
    static char rbuf[PATH_MAX], rtmp[PATH_MAX], rdir[PATH_MAX];
    char name[NAME_MAX + 1];
    const char *cur = path;
    u32 d;
    int last_is_dir, err, depth = 0;

    for (;;) {
        u32 got;
        struct einode ei;

        err = path_walk(cwd_ino(), cur, &d, name, &last_is_dir);
        if (err != 0) {
            return err;
        }
        if (last_is_dir || name[0] == '\0') {
            *ino = d;
            return 0;
        }
        err = dir_lookup(d, name, name_len_of(name), &got, 0);
        if (err != 0) {
            return err;
        }
        err = iread(got, &ei);
        if (err != 0) {
            return err;
        }
        if (!S_ISLNK(ei.mode)) {
            *ino = got;
            return 0;
        }
        if (++depth > SYMLINK_MAX_DEPTH) {
            return -ELOOP;
        }
        err = read_link(got, rbuf, sizeof(rbuf));
        if (err != 0) {
            return err;
        }
        if (rbuf[0] == '/') {
            cur = rbuf;
        } else {
            /* Relative to the DIRECTORY THE LINK IS IN. The walk above
             * left that in `d`, and it is not necessarily the working
             * directory -- `sub/link -> file` means sub/file. */
            u32 dn, bn, k;

            err = ext2_dir_path(d, rdir, sizeof(rdir));
            if (err != 0) {
                return err;
            }
            dn = (u32)strlen(rdir);
            bn = (u32)strlen(rbuf);
            if (dn && rdir[dn - 1] == '/') {
                dn--;           /* the root is "/": do not double it */
            }
            if (dn + 1 + bn + 1 > PATH_MAX) {
                return -ENAMETOOLONG;
            }
            memcpy(rtmp, rdir, dn);
            rtmp[dn] = '/';
            memcpy(rtmp + dn + 1, rbuf, bn);
            rtmp[dn + 1 + bn] = '\0';
            for (k = 0; k <= dn + 1 + bn; k++) {
                rbuf[k] = rtmp[k];
            }
            cur = rbuf;
        }
    }
}

/*
 * The absolute path of a directory, built by walking up through "..".
 *
 * Unlike FAT, where ".." records only the parent's location and the name
 * has to be remembered separately, every ext2 directory can be named
 * from the disk alone: read "..", then find the entry in the parent
 * whose inode is the child.  pwd is therefore always right, including
 * after `cd a/../b` and after another process has renamed something.
 */
static int ext2_dir_path(u32 ino, char *out, u32 size)
{
    char buf[PATH_MAX];
    u32 end = sizeof(buf) - 1;
    u32 cur = ino;
    u32 root = root_ino();
    int depth = 0;

    buf[end] = '\0';
    if (cur == root) {
        if (size < 2) {
            return -ERANGE;
        }
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    while (cur != root) {
        u32 parent, n;
        int i, found = 0;
        char name[NAME_MAX + 1];
        u8 type;
        int err;

        if (++depth > 64) {
            return -ELOOP;
        }
        err = dir_lookup(cur, "..", 2, &parent, &type);
        if (err != 0) {
            return err;
        }
        if (parent == cur) {
            break;              /* the real root, below a chroot       */
        }
        for (i = 0; ; i++) {
            u32 child;

            err = dir_nth(parent, i, name, &child, &type);
            if (err != 0) {
                return -ENOENT;
            }
            if (child == cur && strcmp(name, ".") != 0 &&
                strcmp(name, "..") != 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return -ENOENT;
        }
        n = name_len_of(name);
        if (n + 1 > end) {
            return -ERANGE;
        }
        end -= n;
        memcpy(buf + end, name, n);
        buf[--end] = '/';
        cur = parent;
    }
    if (strlen(buf + end) + 1 > size) {
        return -ERANGE;
    }
    strcpy(out, buf + end);
    if (!out[0]) {
        strcpy(out, "/");
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Mounting                                                          */
/* ---------------------------------------------------------------- */

static u32 find_partition(void)
{
    u8 mbr[SECTOR_SIZE];
    int i;

    if (dev->read(dev, 0, 1, mbr) != 0) {
        return 0;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
        return 0;               /* no partition table: the whole disk */
    }
    for (i = 0; i < 4; i++) {
        const u8 *p = &mbr[446 + i * 16];
        u8 type = p[4];
        u32 start = le32(&p[8]);
        u32 size = le32(&p[12]);

        if (size == 0) {
            continue;
        }
        if (type == 0x83) {     /* Linux                              */
            return start;
        }
    }
    return 0;
}

static void read_label(void)
{
    memcpy(volume_label, sbp(SB_VOLUME_NAME), 16);
    volume_label[16] = '\0';
}

/* The superblock says it is mounted while it is, so that a machine that
 * stopped without unmounting is detectable -- which is what makes the
 * boot-time check run only when it is needed. */
static int set_clean(int clean)
{
    put_le16(sbp(SB_STATE), (u16)(clean ? EXT2_VALID_FS : 0));
    put_le32(sbp(SB_WTIME), now_secs());
    sb_dirty = 1;
    return sb_write();
}

static int ext2_mount_dev(void)
{
    u32 log_bs, compat, incompat, ro_compat, rev;
    u8 *s;

    part_lba = find_partition();
    sectors_per_block = 1;      /* enough to read the superblock       */

    if (sb_read() != 0) {
        return -EIO;
    }
    s = sbuf + EXT2_SUPER_OFF;
    if (le16(s + SB_MAGIC) != EXT2_SUPER_MAGIC) {
        return -EINVAL;
    }

    log_bs = le32(s + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2) {
        return -EINVAL;         /* bigger than 4096: not supported     */
    }
    block_size = 1024u << log_bs;
    sectors_per_block = block_size / SECTOR_SIZE;
    addrs_per_block = block_size / 4;

    incompat = le32(s + SB_FEATURE_INCOMPAT);
    ro_compat = le32(s + SB_FEATURE_RO_COMPAT);
    compat = le32(s + SB_FEATURE_COMPAT);
    if ((incompat & ~(u32)FEAT_INCOMPAT_OK) != 0) {
        return -EOPNOTSUPP;
    }
    if ((ro_compat & ~(u32)FEAT_RO_OK) != 0) {
        return -EOPNOTSUPP;
    }
    if ((compat & FEAT_COMPAT_DIR_INDEX) != 0) {
        return -EOPNOTSUPP;        /* see the note at the top of the file */
    }

    inodes_count = le32(s + SB_INODES_COUNT);
    blocks_count = le32(s + SB_BLOCKS_COUNT);
    r_blocks_count = le32(s + SB_R_BLOCKS_COUNT);
    free_blocks = le32(s + SB_FREE_BLOCKS);
    free_inodes = le32(s + SB_FREE_INODES);
    first_data_block = le32(s + SB_FIRST_DATA_BLOCK);
    blocks_per_group = le32(s + SB_BLOCKS_PER_GROUP);
    inodes_per_group = le32(s + SB_INODES_PER_GROUP);
    rev = le32(s + SB_REV_LEVEL);
    if (rev == 0) {
        inode_size = EXT2_GOOD_OLD_INODE_SIZE;
        first_ino = EXT2_GOOD_OLD_FIRST_INO;
    } else {
        inode_size = le16(s + SB_INODE_SIZE);
        first_ino = le32(s + SB_FIRST_INO);
    }

    if (blocks_per_group == 0 || inodes_per_group == 0 ||
        inode_size < EXT2_GOOD_OLD_INODE_SIZE || inode_size > block_size ||
        (inode_size & (inode_size - 1)) != 0 ||
        blocks_count == 0 || inodes_count == 0 ||
        blocks_per_group > block_size * 8 ||
        inodes_per_group > block_size * 8) {
        return -EINVAL;
    }

    group_count = (blocks_count - first_data_block + blocks_per_group - 1) /
                  blocks_per_group;
    gd_first_block = first_data_block + 1;
    alloc_goal = first_data_block;

    bcache_reset();
    read_label();
    was_unclean = (le16(s + SB_STATE) & EXT2_VALID_FS) ? 0 : 1;
    mounted = 1;

    /* Say so on the disk, so that a machine which stops without
     * unmounting leaves a volume that says it was in use. */
    set_clean(0);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Open files                                                        */
/*                                                                   */
/* A handle is nothing but an inode number: struct file already holds  */
/* the position and the flags, and the inode itself is read through     */
/* the block cache, so two handles on one file see each other's writes  */
/* with no shared structure between them.  The only thing that has to   */
/* be tracked is HOW MANY handles an inode has, because a file unlinked */
/* while it is open must keep its blocks until the last one closes.     */
/* ---------------------------------------------------------------- */

/*
 * One entry per distinct INODE that is open, for the whole machine --
 * not per task. It must not be smaller than vfs.c's FILE_MAX (256), or
 * the machine refuses an open the VFS still has room for, with ENFILE:
 * at 64 that happened on the 65th file and read as a descriptor leak.
 */
#define EXT2_MAX_OPEN   256

static struct {
    u32 ino;
    int refs;
} opened[EXT2_MAX_OPEN];

static int open_ref(u32 ino)
{
    int i, free_slot = -1;

    for (i = 0; i < EXT2_MAX_OPEN; i++) {
        if (opened[i].refs > 0 && opened[i].ino == ino) {
            opened[i].refs++;
            return 0;
        }
        if (opened[i].refs == 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return -ENFILE;
    }
    opened[free_slot].ino = ino;
    opened[free_slot].refs = 1;
    return 0;
}

static int open_count(u32 ino)
{
    int i;

    for (i = 0; i < EXT2_MAX_OPEN; i++) {
        if (opened[i].refs > 0 && opened[i].ino == ino) {
            return opened[i].refs;
        }
    }
    return 0;
}

static void open_unref(u32 ino)
{
    int i;

    for (i = 0; i < EXT2_MAX_OPEN; i++) {
        if (opened[i].refs > 0 && opened[i].ino == ino) {
            opened[i].refs--;
            return;
        }
    }
}

/*
 * Give an inode's blocks back and free the inode itself.  Called when
 * the last link goes and nothing has it open.
 */
static int inode_release(u32 ino)
{
    struct einode ei;
    int err, was_dir;

    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    was_dir = S_ISDIR(ei.mode) ? 1 : 0;
    inode_truncate_blocks(&ei, 0);
    ei.size = 0;
    ei.dtime = now_secs();
    ei.links = 0;
    err = iwrite(&ei);
    inode_free(ino, was_dir);
    return err;
}

/*
 * The orphan list.  An inode whose last name is gone while a program
 * still has it open cannot be freed yet and is no longer reachable from
 * any directory, so the superblock keeps the head of a singly linked
 * list through the inodes' own i_dtime fields -- which is what ext2 has
 * always used the field for in this state.  Mounting walks the list and
 * frees what is on it, so a machine that stopped with an unlinked file
 * open does not leak the blocks and does not need e2fsck to notice.
 */
static int orphan_add(u32 ino)
{
    struct einode ei;
    int err;

    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    ei.dtime = le32(sbp(SB_LAST_ORPHAN));
    err = iwrite(&ei);
    if (err != 0) {
        return err;
    }
    put_le32(sbp(SB_LAST_ORPHAN), ino);
    sb_dirty = 1;
    return 0;
}

static void orphan_remove(u32 ino)
{
    u32 cur = le32(sbp(SB_LAST_ORPHAN));
    struct einode ei;
    u32 prev = 0;

    while (cur != 0 && cur <= inodes_count) {
        if (iread(cur, &ei) != 0) {
            return;
        }
        if (cur == ino) {
            if (prev == 0) {
                put_le32(sbp(SB_LAST_ORPHAN), ei.dtime);
                sb_dirty = 1;
            } else {
                struct einode pe;

                if (iread(prev, &pe) == 0) {
                    pe.dtime = ei.dtime;
                    iwrite(&pe);
                }
            }
            return;
        }
        prev = cur;
        cur = ei.dtime;
    }
}

static void orphan_process(void)
{
    u32 cur = le32(sbp(SB_LAST_ORPHAN));
    int guard = 0;

    while (cur != 0 && cur <= inodes_count && ++guard < 4096) {
        struct einode ei;
        u32 next;

        if (iread(cur, &ei) != 0) {
            break;
        }
        next = ei.dtime;
        if (ei.links == 0) {
            inode_release(cur);
        }
        cur = next;
    }
    put_le32(sbp(SB_LAST_ORPHAN), 0);
    sb_dirty = 1;
}

/* Drop one link from an inode, freeing it when the last one goes and
 * nothing has it open. */
static int drop_link(u32 ino)
{
    struct einode ei;
    int err;

    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (ei.links > 0) {
        ei.links--;
    }
    ei.ctime = now_secs();
    err = iwrite(&ei);
    if (err != 0) {
        return err;
    }
    if (ei.links == 0) {
        if (open_count(ino) > 0) {
            return orphan_add(ino);
        }
        return inode_release(ino);
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* The calls the VFS makes                                           */
/* ---------------------------------------------------------------- */

static int can_write(int flags)
{
    int acc = flags & O_ACCMODE;

    return acc == O_WRONLY || acc == O_RDWR;
}

/* Create a file in `dino` called `name` and return its inode. */
static int make_inode(u32 dino, const char *name, u32 nlen, u16 mode,
                      u8 ftype, u32 *out)
{
    struct einode ei;
    u32 ino, uid = 0, gid = 0;
    int err;

    ino = inode_alloc(S_ISDIR(mode) ? 1 : 0, (dino - 1) / inodes_per_group);
    if (ino == 0) {
        return -ENOSPC;
    }
    err = iclear(ino);
    if (err != 0) {
        inode_free(ino, S_ISDIR(mode) ? 1 : 0);
        return err;
    }
    vfs_cred(&uid, &gid);
    memset(&ei, 0, sizeof(ei));
    ei.ino = ino;
    ei.mode = mode;
    ei.uid = (u16)uid;
    ei.gid = (u16)gid;
    ei.links = 1;
    ei.atime = ei.ctime = ei.mtime = now_secs();
    err = iwrite(&ei);
    if (err != 0) {
        inode_free(ino, S_ISDIR(mode) ? 1 : 0);
        return err;
    }
    err = dir_add(dino, name, nlen, ino, ftype);
    if (err != 0) {
        inode_free(ino, S_ISDIR(mode) ? 1 : 0);
        return err;
    }
    *out = ino;
    return 0;
}

static int ext2_file_close(struct file *f);

static s32 ext2_file_read(struct file *f, void *buf, u32 len)
{
    struct einode ei;
    u32 ino = (u32)f->priv;
    s32 n;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (S_ISDIR(ei.mode)) {
        return -EISDIR;
    }
    n = inode_read(&ei, f->pos, buf, len);
    if (n > 0) {
        f->pos += (u32)n;
    }
    return n;
}

static s32 ext2_file_write(struct file *f, const void *buf, u32 len)
{
    struct einode ei;
    u32 ino = (u32)f->priv;
    s32 n;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (S_ISDIR(ei.mode)) {
        return -EISDIR;
    }
    /* O_APPEND is resolved at every write, not once at open: two
     * handles appending to one log must not overwrite each other. */
    if (f->flags & O_APPEND) {
        f->pos = ei.size;
    }
    n = inode_write(&ei, f->pos, buf, len);
    if (n > 0) {
        f->pos += (u32)n;
        err = iwrite(&ei);
        if (err != 0) {
            return err;
        }
    }
    return n;
}

static s32 ext2_file_lseek(struct file *f, s32 offset, int whence)
{
    struct einode ei;
    u32 ino = (u32)f->priv;
    s32 base;
    int err;

    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (s32)f->pos; break;
    case SEEK_END: base = (s32)ei.size; break;
    default: return -EINVAL;
    }
    if (base + offset < 0) {
        return -EINVAL;
    }
    f->pos = (u32)(base + offset);
    return (s32)f->pos;
}

static void fill_stat(const struct einode *ei, struct stat *st)
{
    st->st_mode = ei->mode;
    st->st_size = ei->size;
    st->st_mtime = ei->mtime;
    st->st_blocks = ei->blocks;
    st->st_ino = ei->ino;
    st->st_uid = ei->uid;
    st->st_gid = ei->gid;
    st->st_nlink = ei->links;
}

static int ext2_file_fstat(struct file *f, struct stat *st)
{
    struct einode ei;
    int err = iread((u32)f->priv, &ei);

    if (err != 0) {
        return err;
    }
    fill_stat(&ei, st);
    return 0;
}

static int ext2_file_truncate(struct file *f, u32 len)
{
    struct einode ei;
    int err;

    if (!can_write(f->flags)) {
        return -EINVAL;         /* Linux's answer for a read-only fd  */
    }
    err = iread((u32)f->priv, &ei);
    if (err != 0) {
        return err;
    }
    if (S_ISDIR(ei.mode)) {
        return -EISDIR;
    }
    err = inode_set_size(&ei, len);
    if (err != 0) {
        return err;
    }
    return iwrite(&ei);
}

static int ext2_file_close(struct file *f)
{
    u32 ino = (u32)f->priv;
    struct einode ei;

    open_unref(ino);
    if (open_count(ino) == 0 && iread(ino, &ei) == 0 && ei.links == 0) {
        orphan_remove(ino);
        inode_release(ino);
    }
    bcache_flush_all();
    sb_write();
    return 0;
}

static const struct file_ops ext2_file_ops = {
    ext2_file_read,
    ext2_file_write,
    ext2_file_lseek,
    0,                          /* no ioctl on a regular file */
    ext2_file_close,
    ext2_file_fstat,
    0,                          /* poll: the default; see dev.h */
    ext2_file_truncate,
    0,                          /* mmap: a file is mapped by mmap.c */
};

static int ext2_open(const char *path, int flags, struct file *f)
{
    char nm[NAME_MAX + 1];
    u32 dino, ino, nlen;
    u8 type;
    int last_is_dir, err;
    struct einode ei;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(cwd_ino(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir) {
        return -EISDIR;         /* "/etc/" is a directory, not a file */
    }
    nlen = name_len_of(nm);

    err = dir_lookup(dino, nm, nlen, &ino, &type);
    if (err == -ENOENT) {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        if (!can_write(flags)) {
            return -EACCES;
        }
        err = make_inode(dino, nm, nlen, (u16)(S_IFREG | 0644),
                         EXT2_FT_REG_FILE, &ino);
        if (err != 0) {
            return err;
        }
    } else if (err != 0) {
        return err;
    } else {
        if (flags & O_EXCL) {
            return -EEXIST;
        }
        err = iread(ino, &ei);
        if (err != 0) {
            return err;
        }
        if (S_ISLNK(ei.mode)) {
            /*
             * FOLLOW IT, which is what opening a symlink means: the
             * link is a way to reach the file, and open(2) opens the
             * file. This used to refuse with ELOOP because nothing
             * could follow one -- so with links working, `cat link`
             * reported "Too many symbolic links" for a link that was
             * perfectly good.
             *
             * O_NOFOLLOW is the caller saying they meant the link
             * itself, and ELOOP is exactly what Linux answers then.
             */
            if (flags & O_NOFOLLOW) {
                return -ELOOP;
            }
            err = path_resolve(path, &ino);
            if (err != 0) {
                return err;
            }
            err = iread(ino, &ei);
            if (err != 0) {
                return err;
            }
            if (S_ISLNK(ei.mode)) {
                return -ELOOP;  /* a chain that never ends */
            }
        }
        if (S_ISDIR(ei.mode)) {
            /* vfs.c decides what opening a directory means and handles
             * the read-only case itself, so this is only reached if
             * something below it changes. Refusing is the safe answer:
             * a directory is not a file this hands out. */
            return -EISDIR;
        }
    }

    err = open_ref(ino);
    if (err != 0) {
        return err;
    }

    if ((flags & O_TRUNC) && can_write(flags)) {
        err = iread(ino, &ei);
        if (err == 0 && ei.size != 0) {
            err = inode_set_size(&ei, 0);
            if (err == 0) {
                err = iwrite(&ei);
            }
        }
        if (err != 0) {
            open_unref(ino);
            return err;
        }
    }

    f->ops = &ext2_file_ops;
    f->priv = (void *)ino;
    f->flags = flags;
    f->pos = 0;
    if (flags & O_APPEND) {
        if (iread(ino, &ei) == 0) {
            f->pos = ei.size;
        }
    }
    return 0;
}

static int ext2_unlink(const char *path)
{
    char nm[NAME_MAX + 1];
    u32 dino, ino, nlen;
    u8 type;
    int last_is_dir, err;
    struct einode ei;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(cwd_ino(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || nm[0] == '\0') {
        return -EISDIR;
    }
    nlen = name_len_of(nm);
    err = dir_lookup(dino, nm, nlen, &ino, &type);
    if (err != 0) {
        return err;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (S_ISDIR(ei.mode)) {
        return -EISDIR;         /* rmdir is the call for those        */
    }
    err = dir_del(dino, nm, nlen, 0);
    if (err != 0) {
        return err;
    }
    err = drop_link(ino);
    if (err != 0) {
        return err;
    }
    bcache_flush_all();
    return sb_write();
}

static int ext2_mkdir(const char *path)
{
    char nm[NAME_MAX + 1];
    u32 dino, ino, nlen, blk, junk;
    u8 type;
    int last_is_dir, err;
    struct einode ei, pe;
    struct bbuf *b;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(cwd_ino(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || nm[0] == '\0') {
        return -EEXIST;
    }
    nlen = name_len_of(nm);
    if (dir_lookup(dino, nm, nlen, &junk, &type) == 0) {
        return -EEXIST;
    }

    err = make_inode(dino, nm, nlen, (u16)(S_IFDIR | 0755), EXT2_FT_DIR,
                     &ino);
    if (err != 0) {
        return err;
    }

    /*
     * "." and ".." are written here and are not decoration: ".." is how
     * a path walk leaves the directory, and how pwd names it.  A
     * directory made without them cannot be left.
     */
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    err = bmap(&ei, 0, 1, &blk);
    if (err != 0) {
        return err;
    }
    b = bget_zero(blk);
    if (!b) {
        return -EIO;
    }
    put_le32(b->data + DE_INODE, ino);
    put_le16(b->data + DE_REC_LEN, 12);
    b->data[DE_NAME_LEN] = 1;
    b->data[DE_FILE_TYPE] = EXT2_FT_DIR;
    b->data[DE_NAME] = '.';

    put_le32(b->data + 12 + DE_INODE, dino);
    put_le16(b->data + 12 + DE_REC_LEN, (u16)(block_size - 12));
    b->data[12 + DE_NAME_LEN] = 2;
    b->data[12 + DE_FILE_TYPE] = EXT2_FT_DIR;
    b->data[12 + DE_NAME] = '.';
    b->data[12 + DE_NAME + 1] = '.';
    bdirty(b);

    ei.size = block_size;
    ei.links = 2;               /* its own name, and its own "."      */
    err = iwrite(&ei);
    if (err != 0) {
        return err;
    }

    /* The parent gains a link through the new directory's "..". */
    err = iread(dino, &pe);
    if (err != 0) {
        return err;
    }
    pe.links++;
    pe.ctime = pe.mtime = now_secs();
    err = iwrite(&pe);
    if (err != 0) {
        return err;
    }
    bcache_flush_all();
    return sb_write();
}

static int ext2_rmdir(const char *path)
{
    char nm[NAME_MAX + 1];
    u32 dino, ino, nlen;
    u8 type;
    int last_is_dir, err;
    struct einode ei, pe;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(cwd_ino(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || nm[0] == '\0') {
        return -EINVAL;         /* "rmdir dir/" names no entry        */
    }
    if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) {
        return -EINVAL;
    }
    nlen = name_len_of(nm);
    err = dir_lookup(dino, nm, nlen, &ino, &type);
    if (err != 0) {
        return err;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (!S_ISDIR(ei.mode)) {
        return -ENOTDIR;
    }
    if (ino == root_ino() || ino == cwd_ino()) {
        return -EBUSY;
    }
    err = dir_is_empty(ino);
    if (err < 0) {
        return err;
    }
    if (err == 0) {
        return -ENOTEMPTY;
    }

    err = dir_del(dino, nm, nlen, 0);
    if (err != 0) {
        return err;
    }
    /* Its own "." was the second link; the name was the first. */
    ei.links = 0;
    ei.ctime = now_secs();
    err = iwrite(&ei);
    if (err != 0) {
        return err;
    }
    if (open_count(ino) > 0) {
        err = orphan_add(ino);
    } else {
        err = inode_release(ino);
    }
    if (err != 0) {
        return err;
    }

    err = iread(dino, &pe);
    if (err != 0) {
        return err;
    }
    if (pe.links > 0) {
        pe.links--;             /* the child's ".." is gone           */
    }
    pe.ctime = pe.mtime = now_secs();
    err = iwrite(&pe);
    if (err != 0) {
        return err;
    }
    bcache_flush_all();
    return sb_write();
}

/* Is `anc` an ancestor of `c`?  Renaming a directory into its own
 * subtree would detach both from the root, so it is refused. */
static int dir_is_within(u32 c, u32 anc)
{
    int guard = 0;

    while (c != root_ino() && ++guard < 256) {
        u32 parent;

        if (c == anc) {
            return 1;
        }
        if (dir_lookup(c, "..", 2, &parent, 0) != 0 || parent == c) {
            break;
        }
        c = parent;
    }
    return c == anc;
}

/*
 * A second name for a file that is already there.
 *
 * WHAT A HARD LINK IS: one inode, two directory entries. There is no
 * original and no copy -- the two names are equal in every way, and
 * the file goes when the LAST of them does, which is what
 * `i_links_count` counts and what drop_link() already honours.
 *
 * DIRECTORIES ARE REFUSED. A link to a directory makes a cycle that
 * `..` cannot describe and that any tree walk -- fsck's included --
 * will follow for ever. Unix forbade it early and only ever let root
 * do it on systems that then regretted allowing it. `.` and `..` are
 * the exception the filesystem writes itself.
 *
 * A link to an inode whose links are already 0 is refused too: that is
 * a file on the orphan list, unlinked but still open, and giving it a
 * new name would resurrect something the kernel has promised to free.
 */
static int ext2_link(const char *from, const char *to)
{
    char fnm[NAME_MAX + 1], tnm[NAME_MAX + 1];
    u32 fdir, tdir, fino, tino, fnl, tnl;
    u8 ftype, ttype;
    int last_is_dir, err;
    struct einode fe;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(cwd_ino(), from, &fdir, fnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || fnm[0] == '\0') {
        return -EINVAL;
    }
    err = path_walk(cwd_ino(), to, &tdir, tnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || tnm[0] == '\0') {
        return -EINVAL;
    }
    fnl = name_len_of(fnm);
    tnl = name_len_of(tnm);

    err = dir_lookup(fdir, fnm, fnl, &fino, &ftype);
    if (err != 0) {
        return err;
    }
    err = iread(fino, &fe);
    if (err != 0) {
        return err;
    }
    if (S_ISDIR(fe.mode)) {
        return -EPERM;          /* never a directory; see above */
    }
    if (fe.links == 0) {
        return -ENOENT;         /* unlinked and still open: let it go */
    }
    /* The new name must not exist. Unlike rename, link never replaces:
     * silently unlinking something to make room for a new name for
     * something else is not what anybody asked for. */
    if (dir_lookup(tdir, tnm, tnl, &tino, &ttype) == 0) {
        return -EEXIST;
    }

    /*
     * The COUNT FIRST, then the entry.
     *
     * If the machine stops between the two, the order decides which
     * kind of damage is left. Count-then-entry leaves a count one too
     * high: fsck sees a link count larger than the names it can find
     * and lowers it, and no data is lost. Entry-then-count leaves a
     * count one too LOW, and then unlinking one of the two names frees
     * an inode the other name still points at -- which is a file that
     * silently becomes somebody else's.
     */
    fe.links++;
    fe.ctime = now_secs();
    err = iwrite(&fe);
    if (err != 0) {
        return err;
    }
    err = dir_add(tdir, tnm, tnl, fino, ftype);
    if (err != 0) {
        /* Put the count back, so a failure leaves nothing behind. */
        fe.links--;
        (void)iwrite(&fe);
        return err;
    }
    return bcache_flush_all();
}

/*
 * Make a symbolic link: a file whose contents are a path.
 *
 * THE TARGET IS NOT CHECKED, and must not be. A symlink to something
 * that does not exist is a DANGLING link, which is legal, useful and
 * common -- it is how a link is made before the thing it points at,
 * and how a link to a removable volume survives the volume being
 * away. Refusing one here would be inventing a rule Unix does not
 * have.
 *
 * Short targets go in the inode (see read_link): no block is
 * allocated, so a symlink normally costs an inode and nothing else.
 */
static int ext2_symlink(const char *target, const char *linkpath)
{
    char nm[NAME_MAX + 1];
    u32 dir, ino, nl, tlen;
    int last_is_dir, err;
    struct einode ei;

    if (!mounted) {
        return -ENODEV;
    }
    tlen = (u32)strlen(target);
    if (tlen == 0) {
        return -ENOENT;         /* an empty target names nothing */
    }
    if (tlen > PATH_MAX - 1) {
        return -ENAMETOOLONG;
    }
    err = path_walk(cwd_ino(), linkpath, &dir, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || nm[0] == '\0') {
        return -EEXIST;
    }
    nl = name_len_of(nm);
    if (dir_lookup(dir, nm, nl, &ino, 0) == 0) {
        return -EEXIST;
    }

    /* 0777 is what Linux gives a symlink, and it means nothing: the
     * permission that decides anything is the TARGET's, and the walk
     * checks that when it gets there. */
    err = make_inode(dir, nm, nl, (u16)(S_IFLNK | 0777),
                     EXT2_FT_SYMLINK, &ino);
    if (err != 0) {
        return err;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (tlen <= SYMLINK_FAST_MAX) {
        u32 i;

        /* Into the block-pointer array, byte by byte and little-endian
         * within each word -- the inverse of read_link, and for the
         * same reason. */
        for (i = 0; i < EXT2_N_BLOCKS; i++) {
            ei.block[i] = 0;
        }
        for (i = 0; i < tlen; i++) {
            ei.block[i / 4] |= (u32)(u8)target[i] << (8 * (i % 4));
        }
        ei.size = tlen;
        ei.blocks = 0;
        err = iwrite(&ei);
    } else {
        s32 n = inode_write(&ei, 0, target, tlen);

        if (n < 0) {
            err = (int)n;
        } else if ((u32)n != tlen) {
            err = -EIO;
        } else {
            err = inode_set_size(&ei, tlen);
            if (err == 0) {
                /*
                 * AND WRITE THE INODE. inode_write and inode_set_size
                 * both work on the in-memory copy and neither calls
                 * iwrite -- so without this the link was created with
                 * size 0 and no blocks, and e2fsck reported "Symlink
                 * /longlink (inode #34) is invalid". The fast path
                 * above writes it; this one forgot to.
                 */
                err = iwrite(&ei);
            }
        }
    }
    if (err != 0) {
        /* Leave nothing behind: the name and the inode both go. */
        (void)dir_del(dir, nm, nl, 0);
        (void)inode_release(ino);
        return err;
    }
    return bcache_flush_all();
}

/* stat, without following a symlink in the last component. */
static int ext2_lstat(const char *path, struct stat *st)
{
    struct einode ei;
    u32 ino;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_resolve_nofollow(path, &ino);
    if (err != 0) {
        return err;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    fill_stat(&ei, st);
    return 0;
}

/* The target of a symlink, without following it. */
static int ext2_readlink(const char *path, char *out, u32 size)
{
    u32 ino;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    /* NOT path_resolve: that follows the last component, and following
     * the link is exactly what readlink must not do. */
    err = path_resolve_nofollow(path, &ino);
    if (err != 0) {
        return err;
    }
    return read_link(ino, out, size);
}

static int ext2_rename(const char *from, const char *to)
{
    char fnm[NAME_MAX + 1], tnm[NAME_MAX + 1];
    u32 fdir, tdir, fino, tino, fnl, tnl;
    u8 ftype, ttype;
    int last_is_dir, err;
    struct einode fe;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(cwd_ino(), from, &fdir, fnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || fnm[0] == '\0') {
        return -EINVAL;
    }
    err = path_walk(cwd_ino(), to, &tdir, tnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || tnm[0] == '\0') {
        return -EINVAL;
    }
    fnl = name_len_of(fnm);
    tnl = name_len_of(tnm);

    err = dir_lookup(fdir, fnm, fnl, &fino, &ftype);
    if (err != 0) {
        return err;
    }
    err = iread(fino, &fe);
    if (err != 0) {
        return err;
    }
    if (fdir == tdir && strcmp(fnm, tnm) == 0) {
        return 0;               /* renaming a thing to itself         */
    }
    if (S_ISDIR(fe.mode) && dir_is_within(tdir, fino)) {
        return -EINVAL;
    }

    /* An existing destination is replaced, which is what rename()
     * promises; a directory may only replace an empty directory. */
    if (dir_lookup(tdir, tnm, tnl, &tino, &ttype) == 0) {
        struct einode te;

        if (tino == fino) {
            return 0;
        }
        err = iread(tino, &te);
        if (err != 0) {
            return err;
        }
        if (S_ISDIR(te.mode)) {
            if (!S_ISDIR(fe.mode)) {
                return -EISDIR;
            }
            err = dir_is_empty(tino);
            if (err < 0) {
                return err;
            }
            if (err == 0) {
                return -ENOTEMPTY;
            }
        } else if (S_ISDIR(fe.mode)) {
            return -ENOTDIR;
        }
        err = dir_del(tdir, tnm, tnl, 0);
        if (err != 0) {
            return err;
        }
        if (S_ISDIR(te.mode)) {
            struct einode pe;

            te.links = 0;
            iwrite(&te);
            if (open_count(tino) > 0) {
                orphan_add(tino);
            } else {
                inode_release(tino);
            }
            if (iread(tdir, &pe) == 0 && pe.links > 0) {
                pe.links--;
                iwrite(&pe);
            }
        } else {
            err = drop_link(tino);
            if (err != 0) {
                return err;
            }
        }
    }

    err = dir_add(tdir, tnm, tnl, fino, ftype);
    if (err != 0) {
        return err;
    }
    err = dir_del(fdir, fnm, fnl, 0);
    if (err != 0) {
        return err;
    }

    /* A directory that moved to a different parent carries its ".."
     * with it, and the link counts on both parents follow. */
    if (S_ISDIR(fe.mode) && fdir != tdir) {
        struct einode oe, ne;
        u32 blk;
        struct bbuf *b;

        err = iread(fino, &fe);
        if (err == 0 && bmap(&fe, 0, 0, &blk) == 0 && blk != 0) {
            b = bget(blk);
            if (b) {
                u32 p = le16(b->data + DE_REC_LEN);

                if (p + DE_MIN_SIZE <= block_size &&
                    b->data[p + DE_NAME_LEN] == 2 &&
                    b->data[p + DE_NAME] == '.' &&
                    b->data[p + DE_NAME + 1] == '.') {
                    put_le32(b->data + p + DE_INODE, tdir);
                    bdirty(b);
                }
            }
        }
        if (iread(fdir, &oe) == 0 && oe.links > 0) {
            oe.links--;
            oe.ctime = oe.mtime = now_secs();
            iwrite(&oe);
        }
        if (iread(tdir, &ne) == 0) {
            ne.links++;
            ne.ctime = ne.mtime = now_secs();
            iwrite(&ne);
        }
    }

    if (iread(fino, &fe) == 0) {
        fe.ctime = now_secs();
        iwrite(&fe);
    }
    bcache_flush_all();
    return sb_write();
}

static int ext2_stat(const char *path, struct stat *st)
{
    struct einode ei;
    u32 ino;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_resolve(path, &ino);
    if (err != 0) {
        return err;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    fill_stat(&ei, st);
    return 0;
}

static void fill_dirent(const char *name, u32 ino, const struct einode *ei,
                        struct dirent *out)
{
    u32 n = name_len_of(name);

    if (n > NAME_MAX) {
        n = NAME_MAX;
    }
    memcpy(out->d_name, name, n);
    out->d_name[n] = '\0';
    out->d_size = ei->size;
    out->d_mode = ei->mode;
    out->d_mtime = ei->mtime;
    out->d_ino = ino;
}

static int ext2_readdir_in(u32 ino, int index, struct dirent *out)
{
    char name[NAME_MAX + 1];
    struct einode ei;
    u32 child;
    u8 type;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    if (ino == 0) {
        ino = root_ino();
    }
    err = dir_nth(ino, index, name, &child, &type);
    if (err != 0) {
        return err;
    }
    err = iread(child, &ei);
    if (err != 0) {
        return err;
    }
    fill_dirent(name, child, &ei, out);
    return 0;
}

static int ext2_readdir(int index, struct dirent *out)
{
    return ext2_readdir_in(cwd_ino(), index, out);
}

static int ext2_dir_ino(const char *path, u32 *ino)
{
    struct einode ei;
    u32 n;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_resolve(path, &n);
    if (err != 0) {
        return err;
    }
    err = iread(n, &ei);
    if (err != 0) {
        return err;
    }
    if (!S_ISDIR(ei.mode)) {
        return -ENOTDIR;
    }
    *ino = n;
    return 0;
}

static int ext2_dir_path_op(u32 ino, char *out, u32 size)
{
    if (!mounted) {
        return -ENODEV;
    }
    if (ino == 0) {
        ino = root_ino();
    }
    return ext2_dir_path(ino, out, size);
}

static int ext2_chdir(const char *path)
{
    char cwd_path[PATH_MAX];
    struct einode ei;
    u32 ino;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_resolve(path, &ino);
    if (err != 0) {
        return err;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (!S_ISDIR(ei.mode)) {
        return -ENOTDIR;
    }
    err = ext2_dir_path(ino, cwd_path, sizeof(cwd_path));
    if (err != 0) {
        return err;
    }
    vfs_cwd_set(ino, cwd_path);
    return 0;
}

static const char *ext2_getcwd(void)
{
    return vfs_cwd_path();
}

static int ext2_statfs(struct statfs *s)
{
    if (!mounted) {
        return -ENODEV;
    }
    memset(s, 0, sizeof(*s));
    s->f_type = EXT2_SUPER_MAGIC;
    s->f_bsize = block_size;
    s->f_frsize = block_size;
    s->f_blocks = blocks_count;
    s->f_bfree = free_blocks;
    s->f_bavail = (free_blocks > r_blocks_count) ?
                  free_blocks - r_blocks_count : 0;
    s->f_files = inodes_count;
    s->f_ffree = free_inodes;
    s->f_namelen = 255;
    s->f_fsid[0] = le32(sbp(SB_UUID));
    s->f_fsid[1] = le32(sbp(SB_UUID + 4));
    return 0;
}

static int ext2_label(struct fslabel *l)
{
    u32 i;

    if (!mounted) {
        return -ENODEV;
    }
    for (i = 0; i < sizeof(l->name) - 1 && i < 16; i++) {
        l->name[i] = volume_label[i];
    }
    l->name[i] = '\0';
    return 0;
}

static int ext2_sync(void)
{
    int a, b;

    if (!mounted) {
        return 0;
    }
    a = bcache_flush_all();
    b = sb_write();
    return (a != 0) ? a : b;
}

/*
 * 0xffffffff means UTIME_OMIT: leave that one alone. The kernel's
 * utimensat spells it that way (syslinux.c) and fs/fat16.c reads it the
 * same, so a filesystem that assigned both unconditionally would set a
 * time the caller explicitly asked it not to touch.
 */
static int set_times(u32 ino, u32 mtime, u32 atime)
{
    struct einode ei;
    int err = iread(ino, &ei);

    if (err != 0) {
        return err;
    }
    if (mtime != 0xffffffffUL) {
        ei.mtime = mtime;
    }
    if (atime != 0xffffffffUL) {
        ei.atime = atime;
    }
    ei.ctime = now_secs();
    err = iwrite(&ei);
    if (err != 0) {
        return err;
    }
    return bcache_flush_all();
}

/*
 * Mode and ownership. Only the bits `mask` names are touched.
 *
 * The file TYPE bits of i_mode are kept whatever the caller passed:
 * chmod(2) takes a mode with them zeroed, and letting them through
 * would let a chmod turn a directory into a regular file on disk --
 * which e2fsck reports and the kernel would then refuse to open.
 * (fsimg.sh met the other half of this: `sif <file> mode 0755` sets the
 * WHOLE of i_mode, so it has to be written 0100755.)
 */
static int set_attr(u32 ino, u32 mask, u32 mode, u32 uid, u32 gid)
{
    struct einode ei;
    int err = iread(ino, &ei);

    if (err != 0) {
        return err;
    }
    if (mask & ATTR_MODE) {
        ei.mode = (u16)((ei.mode & S_IFMT) | (mode & 07777));
    }
    if (mask & ATTR_UID) {
        ei.uid = (u16)uid;
    }
    if (mask & ATTR_GID) {
        ei.gid = (u16)gid;
    }
    /* ctime is "when the inode last changed", which is exactly this. */
    ei.ctime = now_secs();
    err = iwrite(&ei);
    if (err != 0) {
        return err;
    }
    return bcache_flush_all();
}

static int ext2_setattr(const char *path, u32 mask, u32 mode, u32 uid, u32 gid)
{
    u32 ino;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_resolve(path, &ino);
    if (err != 0) {
        return err;
    }
    return set_attr(ino, mask, mode, uid, gid);
}

static int ext2_fsetattr(struct file *f, u32 mask, u32 mode, u32 uid, u32 gid)
{
    if (!mounted) {
        return -ENODEV;
    }
    return set_attr((u32)f->priv, mask, mode, uid, gid);
}

static int ext2_utime(const char *path, u32 mtime, u32 atime)
{
    u32 ino;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_resolve(path, &ino);
    if (err != 0) {
        return err;
    }
    return set_times(ino, mtime, atime);
}

static int ext2_futime(struct file *f, u32 mtime, u32 atime)
{
    if (!mounted) {
        return -ENODEV;
    }
    return set_times((u32)f->priv, mtime, atime);
}

/*
 * The disk block behind a byte offset of an open file, for the swap
 * file and for anything else that wants to reach the device directly.
 */
static int ext2_bmap(struct file *f, u32 off, u32 *lba, struct blockdev **d)
{
    struct einode ei;
    u32 blk;
    int err;

    if (!mounted) {
        return -ENODEV;
    }
    err = iread((u32)f->priv, &ei);
    if (err != 0) {
        return err;
    }
    if (off >= ei.size) {
        return -EINVAL;
    }
    err = bmap(&ei, off / block_size, 0, &blk);
    if (err != 0) {
        return err;
    }
    if (blk == 0) {
        return -EINVAL;         /* a hole has no block to name        */
    }
    *lba = part_lba + blk * sectors_per_block +
           (off % block_size) / SECTOR_SIZE;
    *d = dev;
    return 0;
}

/* ---------------------------------------------------------------- */
/* The consistency check                                             */
/*                                                                   */
/* What it establishes, in the order it establishes it:               */
/*                                                                    */
/*   every inode the inode bitmaps say is in use has a block map whose */
/*     pointers are inside the volume and shared with no other inode   */
/*   every block a live inode reaches is marked in use                 */
/*   every block marked in use is reached by something                 */
/*   every directory entry points at an inode that is in use           */
/*   every directory's "." is itself and ".." is its parent            */
/*   every inode's link count equals the number of names that reach it */
/*   the free counts in the superblock and the group descriptors equal */
/*     what the bitmaps actually say                                   */
/*                                                                    */
/* With `repair`, the counts are corrected, blocks that nothing reaches */
/* are given back, and an in-use inode that no name reaches and whose   */
/* link count is zero -- the orphan of a machine that stopped with a    */
/* deleted file still open -- is freed.                                */
/*                                                                    */
/* The working set is carved out of one fixed arena rather than sized   */
/* per volume, because there is no allocator here.  A volume too big    */
/* for it is reported as unchecked rather than half checked.            */
/* ---------------------------------------------------------------- */

#define FSCK_ARENA      (192 * 1024)
#define FSCK_MAX_DEPTH  64

static u8 fsck_arena[FSCK_ARENA];
static u8 *seen_map;            /* one bit per block: reached          */
/*
 * One byte per inode, holding the number of DIRECTORY ENTRIES that
 * account for its link count: the names that point at it, plus -- for a
 * directory -- one for each subdirectory, whose ".." is a link back.
 * Its own "." is the constant 1 added at the end.
 *
 * Both are kept in the one array on purpose. A second array of the same
 * size does not fit: on the 512 MB disk that is 128 K inodes, and two
 * of them plus the block bitmap is 272 KB of an arena that has to be
 * static because there is no allocator here.
 */
static u8 *links_map;
static u32 links_cap;

static int seen_test(u32 b)
{
    return (seen_map[b >> 3] >> (b & 7)) & 1;
}

static void seen_set(u32 b)
{
    seen_map[b >> 3] |= (u8)(1u << (b & 7));
}

/*
 * Walk one branch of an inode's block map, marking every block it
 * reaches. Returns 1 when the POINTER THAT LED HERE should be cleared:
 * it is out of range, or the block is already claimed by something
 * else. The caller owns that pointer and is the only one that can zero
 * it, which is why the answer comes back rather than being acted on
 * here.
 */
static int fsck_walk_blocks(u32 blk, int level, struct fsck_report *r,
                            u32 *count, int repair)
{
    struct bbuf *b;
    u32 i;

    if (blk == 0) {
        return 0;
    }
    if (blk < first_data_block || blk >= blocks_count) {
        r->bad_blocks++;
        return 1;               /* out of range: cut it off           */
    }
    if (seen_test(blk)) {
        r->cross_linked++;
        return 1;               /* somebody else has it already       */
    }
    seen_set(blk);
    (*count)++;
    if (level == 0) {
        return 0;
    }
    for (i = 0; i < addrs_per_block; i++) {
        u32 entry;

        b = bget(blk);
        if (!b) {
            return 0;
        }
        entry = le32(b->data + 4 * i);
        if (entry == 0) {
            continue;
        }
        if (fsck_walk_blocks(entry, level - 1, r, count, repair) && repair) {
            b = bget(blk);      /* the recursion may have evicted it  */
            if (b) {
                put_le32(b->data + 4 * i, 0);
                bdirty(b);
                r->fixed++;
            }
        }
    }
    return 0;
}

/* The whole of one inode's map. Clears any pointer the walk condemns,
 * and says whether the inode itself needs writing back. */
static int fsck_inode_blocks(struct einode *ei, struct fsck_report *r,
                             u32 *count, int repair)
{
    int i, dirty = 0;
    static const int level_of[3] = { 1, 2, 3 };

    for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (fsck_walk_blocks(ei->block[i], 0, r, count, repair) && repair) {
            ei->block[i] = 0;
            dirty = 1;
        }
    }
    for (i = 0; i < 3; i++) {
        int slot = EXT2_IND_BLOCK + i;

        if (fsck_walk_blocks(ei->block[slot], level_of[i], r, count,
                             repair) && repair) {
            ei->block[slot] = 0;
            dirty = 1;
        }
    }
    return dirty;
}

static int inode_in_use(u32 ino)
{
    u32 g = (ino - 1) / inodes_per_group;
    u32 bit = (ino - 1) % inodes_per_group;
    struct bbuf *b = bget(gd_field(g, GD_INODE_BITMAP));

    return b ? bitmap_test(b->data, bit) : 0;
}

static void inode_mark(u32 ino, int value)
{
    u32 g = (ino - 1) / inodes_per_group;
    u32 bit = (ino - 1) % inodes_per_group;
    struct bbuf *b = bget(gd_field(g, GD_INODE_BITMAP));

    if (!b) {
        return;
    }
    if (value) {
        bitmap_set(b->data, bit);
    } else {
        bitmap_clear(b->data, bit);
    }
    bdirty(b);
}

/*
 * Is this inode LIVE?
 *
 * The bitmap is one opinion and the inode itself is another, and they
 * can disagree: an inode with a link count and no deletion time holds a
 * file whatever the bitmap says. Trusting the bitmap alone means
 * walking past such an inode, deciding the blocks it points at are
 * reached by nothing, and freeing them out from under a file that still
 * has them -- which is what a check is supposed to prevent. e2fsck
 * reads the table for the same reason.
 */
static int fsck_inode_live(u32 ino, struct einode *ei, int *in_bitmap)
{
    int bit = inode_in_use(ino);

    *in_bitmap = bit;
    if (iread(ino, ei) != 0) {
        return bit;
    }
    if (bit) {
        return 1;
    }
    return (ei->links > 0 && ei->dtime == 0);
}

/*
 * Set every group's free counts to what its bitmaps actually say, and
 * report the totals.
 *
 * The totals have to come from HERE rather than from the sweep above,
 * because repairing can itself allocate: reconnecting an inode to
 * /lost+found may need a block for that directory, and it happens after
 * the sweep has counted. Setting the superblock from the sweep's figure
 * would then leave it one block too high -- which e2fsck reports and
 * which would only ever show up on a volume damaged in that particular
 * way.
 */
static void fsck_recount_groups(u32 *free_b, u32 *free_i)
{
    u32 g;

    *free_b = 0;
    *free_i = 0;

    for (g = 0; g < group_count; g++) {
        struct bbuf *b;
        u32 i, limit, freeb = 0, freei = 0, dirs = 0;
        u8 *d;
        struct bbuf *gb;

        b = bget(gd_field(g, GD_BLOCK_BITMAP));
        if (!b) {
            return;
        }
        limit = group_blocks(g);
        for (i = 0; i < limit; i++) {
            if (!bitmap_test(b->data, i)) {
                freeb++;
            }
        }
        /*
         * TWO PASSES, and the second re-fetches the bitmap every time.
         *
         * iread() goes through the same sixteen-buffer cache, so it can
         * EVICT the bitmap buffer -- and a pointer into it then points
         * at whatever block took its place. Counting free bits and
         * reading inodes in one loop therefore counted the first part
         * of the bitmap and then some inode table, which is how a
         * repair came to write a free-inode count that was wrong by
         * exactly the number of inodes it had read.
         */
        b = bget(gd_field(g, GD_INODE_BITMAP));
        if (!b) {
            return;
        }
        for (i = 0; i < inodes_per_group; i++) {
            if (!bitmap_test(b->data, i)) {
                freei++;
            }
        }
        for (i = 0; i < inodes_per_group; i++) {
            struct einode ei;

            b = bget(gd_field(g, GD_INODE_BITMAP));
            if (!b) {
                return;
            }
            if (!bitmap_test(b->data, i)) {
                continue;
            }
            if (iread(g * inodes_per_group + i + 1, &ei) == 0 &&
                S_ISDIR(ei.mode)) {
                dirs++;
            }
        }
        d = gd_get(g, &gb);
        if (!d) {
            return;
        }
        put_le16(d + GD_FREE_BLOCKS, (u16)freeb);
        put_le16(d + GD_FREE_INODES, (u16)freei);
        put_le16(d + GD_USED_DIRS, (u16)dirs);
        bdirty(gb);
        *free_b += freeb;
        *free_i += freei;
    }
}

/*
 * Put a directory's "." and ".." back. They are ordinary entries and
 * are found by name rather than by position, because a directory that
 * came from somewhere else may not have laid them out the way mkdir
 * here does.
 */
static int fsck_fix_dots(u32 dino, u32 parent)
{
    struct einode di;
    u32 off;
    int fixed = 0;

    if (iread(dino, &di) != 0) {
        return 0;
    }
    for (off = 0; off < di.size; off += block_size) {
        struct bbuf *b;
        u32 blk, p;

        if (bmap(&di, off / block_size, 0, &blk) != 0 || blk == 0) {
            continue;
        }
        b = bget(blk);
        if (!b) {
            return fixed;
        }
        p = 0;
        while (p + DE_MIN_SIZE <= block_size) {
            u8 *ent = b->data + p;
            u32 rec = le16(ent + DE_REC_LEN);
            u32 nl = ent[DE_NAME_LEN];
            u32 want = 0;

            if (rec < DE_MIN_SIZE || p + rec > block_size) {
                break;
            }
            if (nl == 1 && ent[DE_NAME] == '.') {
                want = dino;
            } else if (nl == 2 && ent[DE_NAME] == '.' &&
                       ent[DE_NAME + 1] == '.') {
                want = parent;
            }
            if (want != 0 && le32(ent + DE_INODE) != want) {
                put_le32(ent + DE_INODE, want);
                ent[DE_FILE_TYPE] = EXT2_FT_DIR;
                bdirty(b);
                fixed++;
            }
            p += rec;
        }
    }
    return fixed;
}

/*
 * Give an inode that no name reaches a name again, in /lost+found,
 * called after its own number. Freeing it instead would be throwing
 * away a file whose only fault is that its directory entry went.
 */
static int fsck_reconnect(u32 ino)
{
    struct einode ei;
    char name[16];
    u32 lf, n = ino;
    int i = 0, j;
    u8 type;

    if (dir_lookup(root_ino(), "lost+found", 10, &lf, &type) != 0) {
        return 0;
    }
    if (iread(ino, &ei) != 0) {
        return 0;
    }
    name[i++] = '#';
    {
        char d[12];
        int k = 0;

        if (n == 0) {
            d[k++] = '0';
        }
        while (n > 0) {
            d[k++] = (char)('0' + (n % 10));
            n /= 10;
        }
        for (j = k - 1; j >= 0; j--) {
            name[i++] = d[j];
        }
    }
    name[i] = '\0';
    if (dir_add(lf, name, (u32)i, ino,
                S_ISDIR(ei.mode) ? EXT2_FT_DIR : EXT2_FT_REG_FILE) != 0) {
        return 0;
    }
    if (ei.links == 0) {
        ei.links = 1;
        ei.ctime = now_secs();
        iwrite(&ei);
    }
    return 1;
}

/* Depth-first over the directory tree, counting names per inode and
 * checking "." and "..". */
static int fsck_repairing;      /* set for the length of one check    */

/*
 * Depth-first over the directory tree, counting the entries that
 * account for each inode's link count and checking "." and "..".
 *
 * It walks a directory's blocks DIRECTLY rather than asking dir_nth()
 * for entry 0, 1, 2 and so on: dir_nth restarts at the first entry
 * every time, so using it here is quadratic in the size of each
 * directory. With the compiler installed that took a hundred seconds
 * to check the disk at boot -- which reads as a machine that has hung,
 * because the banner stops after the mount line and nothing else is
 * printed until it finishes.
 *
 * The buffer is fetched again on every entry because the recursion
 * reads other blocks and may have evicted this one.
 */
static void fsck_tree(u32 dino, u32 parent, int depth, struct fsck_report *r)
{
    struct einode di;
    u32 off;
    int bad_dots = 0;

    if (depth > FSCK_MAX_DEPTH) {
        r->too_deep++;
        return;
    }
    if (iread(dino, &di) != 0 || !S_ISDIR(di.mode)) {
        return;
    }

    for (off = 0; off < di.size; off += block_size) {
        u32 blk, p = 0;

        if (bmap(&di, off / block_size, 0, &blk) != 0 || blk == 0) {
            continue;
        }
        while (p + DE_MIN_SIZE <= block_size) {
            struct bbuf *b = bget(blk);
            char name[NAME_MAX + 1];
            struct einode ce;
            u8 *ent;
            u32 rec, child, nl;

            if (!b) {
                return;
            }
            ent = b->data + p;
            rec = le16(ent + DE_REC_LEN);
            if (rec < DE_MIN_SIZE || p + rec > block_size) {
                break;
            }
            child = le32(ent + DE_INODE);
            nl = ent[DE_NAME_LEN];
            if (nl > NAME_MAX) {
                nl = NAME_MAX;
            }
            memcpy(name, ent + DE_NAME, nl);
            name[nl] = '\0';
            p += rec;           /* advance BEFORE anything may recurse */

            if (child == 0) {
                continue;
            }
            if (nl == 1 && name[0] == '.') {
                if (child != dino) {
                    r->dot_entries++;
                    bad_dots = 1;
                }
                continue;       /* not a name for counting purposes   */
            }
            if (nl == 2 && name[0] == '.' && name[1] == '.') {
                if (child != parent) {
                    r->dot_entries++;
                    bad_dots = 1;
                }
                continue;
            }
            if (child > inodes_count || !inode_in_use(child)) {
                r->orphan_names++;
                continue;
            }
            if (child < links_cap && links_map[child] < 255) {
                links_map[child]++;
            }
            if (iread(child, &ce) != 0) {
                continue;
            }
            if (S_ISDIR(ce.mode)) {
                r->dirs++;
                /* A subdirectory's ".." is a link back to here, and is
                 * counted here rather than by walking this directory
                 * again per entry in the sweep below. */
                if (dino < links_cap && links_map[dino] < 255) {
                    links_map[dino]++;
                }
                /* Reached by exactly one name, so a second visit would
                 * be a loop; the link count check catches that. */
                if (child != dino && child < links_cap &&
                    links_map[child] == 1) {
                    fsck_tree(child, dino, depth + 1, r);
                }
            } else {
                r->files++;
            }
        }
    }
    if (bad_dots && fsck_repairing) {
        r->fixed += (u32)fsck_fix_dots(dino, parent);
    }
}

static int ext2_check(int flags, struct fsck_report *r)
{
    u32 seen_bytes, g, ino, b, used = 0, counted_free = 0;
    u32 inodes_per_block = block_size / inode_size;
    int repair = (flags & FSCK_REPAIR) != 0;
    int i, orphan_count = 0;
    /* Inodes no name reaches, dealt with after the block sweep. A disk
     * with more than this many is one for e2fsck, not for us. */
    static u32 orphans[64];
    struct einode ei;

    if (!mounted) {
        return -ENODEV;
    }
    memset(r, 0, sizeof(*r));
    r->block_bytes = block_size;
    r->was_dirty = was_unclean;

    /*
     * FSCK_IF_DIRTY means "only if the volume was not unmounted
     * cleanly", and honouring it is not optional: main.c calls this at
     * boot with FSCK_IF_DIRTY|FSCK_REPAIR and prints nothing when the
     * volume was clean. A check that ran anyway would silently REPAIR a
     * clean-looking volume at every boot -- which is how damage planted
     * for a test came to be gone before the test could look for it.
     */
    if ((flags & FSCK_IF_DIRTY) && !was_unclean) {
        return 0;
    }
    /* Repairing a file that something has open would pull its blocks
     * out from under the handle. */
    if (repair) {
        int k;

        for (k = 0; k < EXT2_MAX_OPEN; k++) {
            if (opened[k].refs > 0) {
                return -EBUSY;
            }
        }
    }

    seen_bytes = (blocks_count + 7) / 8;
    if (seen_bytes + inodes_count + 1 > FSCK_ARENA) {
        return -EFBIG;          /* too big to check here; say so       */
    }
    memset(fsck_arena, 0, seen_bytes + inodes_count + 1);
    seen_map = fsck_arena;
    links_map = fsck_arena + seen_bytes;
    links_cap = inodes_count + 1;

    /* The metadata of every group is in use by definition. */
    for (g = 0; g < group_count; g++) {
        u32 itab = gd_field(g, GD_INODE_TABLE);
        u32 n = (inodes_per_group * inode_size + block_size - 1) / block_size;
        u32 k;

        seen_set(gd_field(g, GD_BLOCK_BITMAP));
        seen_set(gd_field(g, GD_INODE_BITMAP));
        for (k = 0; k < n; k++) {
            seen_set(itab + k);
        }
    }
    /* The superblock, the descriptors and any backups of them: rather
     * than work out which groups keep backups, take every block below
     * the first group's bitmap as metadata, per group. */
    for (g = 0; g < group_count; g++) {
        u32 base = first_data_block + g * blocks_per_group;
        u32 bm = gd_field(g, GD_BLOCK_BITMAP);
        u32 k;

        for (k = base; k < bm && k < blocks_count; k++) {
            seen_set(k);
        }
    }

    /*
     * The tree first: it fills links_map, which the ONE pass over the
     * inode table below needs, and it reads only directories.
     */
    r->dirs = 0;
    r->files = 0;
    if (links_cap > EXT2_ROOT_INO) {
        links_map[EXT2_ROOT_INO] = 1;
    }
    fsck_repairing = repair;
    fsck_tree(root_ino(), root_ino(), 0, r);
    fsck_repairing = 0;

    /* Every inode that is in use: its blocks, and its type. */
    for (ino = 1; ino <= inodes_count; ino++) {
        u32 count = 0;
        int in_bitmap;

        /*
         * A WHOLE TABLE BLOCK THE BITMAP SAYS IS EMPTY IS NOT READ.
         *
         * Reading the inode table is what a check costs: 32 MB on the
         * 512 MB disk, one 4 KB request at a time, and the latency of
         * eight thousand of those was a minute of boot after an unclean
         * stop. Almost all of that table is free space on any real
         * disk.
         *
         * What it gives up: an inode holding a live file in a block
         * where the bitmap has ALL sixteen marked free would not be
         * seen. One bit wrongly cleared beside an inode that IS in use
         * still is, which is the shape that damage actually takes here;
         * a whole block of them is e2fsck's problem, and e2fsck reads
         * every inode.
         */
        if (((ino - 1) % inodes_per_block) == 0) {
            u32 k, any = 0;

            for (k = 0; k < inodes_per_block && ino + k <= inodes_count; k++) {
                if (inode_in_use(ino + k)) {
                    any = 1;
                    break;
                }
            }
            if (!any) {
                u32 skip = inodes_per_block;

                if (ino + skip - 1 > inodes_count) {
                    skip = inodes_count - ino + 1;
                }
                counted_free += skip;
                ino += skip - 1;
                continue;
            }
        }

        if (!fsck_inode_live(ino, &ei, &in_bitmap)) {
            counted_free++;
            continue;
        }
        if (!in_bitmap) {
            /* The table says it holds a file and the bitmap says it is
             * free. Believe the table, or its blocks get given away. */
            r->count_mismatch++;
            if (repair) {
                inode_mark(ino, 1);
                r->fixed++;
            }
        }
        if (fsck_inode_blocks(&ei, r, &count, repair)) {
            /*
             * Cutting a pointer off changes how many blocks the inode
             * owns, and i_blocks has to follow or the volume still does
             * not check ("i_blocks is 80, should be 72"). The walk
             * counted every block it reached, data and indirect alike,
             * which is exactly what i_blocks means -- in 512-byte
             * units, whatever the block size is.
             *
             * Only done when something WAS cut: an inode carrying an
             * extended-attribute block owns one more than the walk can
             * see, and recomputing unconditionally would "fix" that
             * into a fault.
             */
            u32 want = count * (block_size / 512);

            if (ei.blocks != want) {
                r->size_fixed++;
                ei.blocks = want;
            }
            iwrite(&ei);
        }
        if (S_ISDIR(ei.mode) && ei.size % block_size != 0) {
            r->size_fixed++;
            if (repair) {
                ei.size = ((ei.size + block_size - 1) / block_size) *
                          block_size;
                iwrite(&ei);
                r->fixed++;
            }
        }

        /*
         * The link count, in the same pass. Reading the inode table is
         * what a check costs -- 32 MB of it on the 512 MB disk -- and
         * doing this in a second loop read the whole table twice, which
         * was most of the eighty seconds an unclean boot took.
         */
        if (ino >= first_ino || ino == EXT2_ROOT_INO) {
            u32 names = (ino < links_cap) ? links_map[ino] : 0;

            if (S_ISDIR(ei.mode)) {
                names = names + 1;  /* its own "."; the rest is counted */
            }
            if (names == 0) {
                r->unattached++;
                if (repair) {
                    /* Acted on AFTER the block sweep below: giving it a
                     * name may have to grow /lost+found, and a block
                     * allocated now is a block the sweep would find
                     * marked in use and reached by nothing. */
                    if (orphan_count < (int)(sizeof(orphans) /
                                             sizeof(orphans[0]))) {
                        orphans[orphan_count++] = ino;
                    }
                }
            } else if (ei.links != names) {
                r->bad_links++;
                if (repair) {
                    ei.links = (u16)names;
                    iwrite(&ei);
                    r->fixed++;
                }
            }
        }
        /* Inodes below first_ino are the reserved ones; no name
         * reaches them and none is meant to. */
    }

    /* Blocks: marked in use but reached by nothing, or the reverse. */
    for (b = first_data_block; b < blocks_count; b++) {
        u32 grp = (b - first_data_block) / blocks_per_group;
        u32 bit = (b - first_data_block) % blocks_per_group;
        struct bbuf *bm = bget(gd_field(grp, GD_BLOCK_BITMAP));
        int in_map;

        if (!bm) {
            break;
        }
        in_map = bitmap_test(bm->data, bit);
        if (in_map) {
            used++;
        }
        if (in_map && !seen_test(b)) {
            r->lost_blocks++;
            if (repair) {
                block_free(b);
                used--;
                r->fixed++;
            }
        } else if (!in_map && seen_test(b)) {
            r->bad_blocks++;
            if (repair) {
                struct bbuf *again = bget(gd_field(grp, GD_BLOCK_BITMAP));

                if (again) {
                    bitmap_set(again->data, bit);
                    bdirty(again);
                    gd_add_free_blocks(grp, -1);
                    free_blocks--;
                    used++;
                    r->fixed++;
                }
            }
        }
    }
    r->blocks_used = used;
    r->blocks_free = blocks_count - used;

    /*
     * The orphans, now that the block sweep has finished: each gets a
     * name in /lost+found, or is freed if it cannot have one.
     */
    for (i = 0; i < orphan_count; i++) {
        struct einode oe;

        if (iread(orphans[i], &oe) != 0) {
            continue;
        }
        if ((oe.links == 0 && oe.size == 0) || !fsck_reconnect(orphans[i])) {
            inode_release(orphans[i]);
        }
        r->fixed++;
    }

    /* The counts the superblock keeps, against what the bitmaps say. */
    if (free_blocks != r->blocks_free || free_inodes != counted_free) {
        r->count_mismatch++;
    }

    if (repair) {
        u32 fb, fi;

        /* The per-group counts are a summary of the bitmaps and are
         * not repaired by fixing the superblock's totals: e2fsck
         * reports them separately ("Free inodes count wrong for group
         * #0"), and a volume with the totals right and a group wrong
         * is still a volume that does not check. */
        fsck_recount_groups(&fb, &fi);
        if (free_blocks != fb || free_inodes != fi) {
            free_blocks = fb;
            free_inodes = fi;
            r->fixed++;
        }
        sb_set_free_counts();
        bcache_flush_all();
        sb_write();
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Mount, unmount, and the type itself                               */
/* ---------------------------------------------------------------- */

static int ext2_mount(struct blockdev *b)
{
    int err;

    if (!b || !b->read) {
        return -ENXIO;
    }
    if (b->sector_size != SECTOR_SIZE) {
        return -EINVAL;
    }
    dev = b;
    memset(opened, 0, sizeof(opened));
    err = ext2_mount_dev();
    if (err < 0) {
        dev = 0;
        mounted = 0;
        return err;
    }
    /* Anything left on the orphan list belongs to a run that stopped
     * with a deleted file still open.  Free it now rather than leave it
     * for a check that may never be run. */
    orphan_process();
    bcache_flush_all();
    sb_write();
    return 0;
}

static int ext2_umount(void)
{
    if (mounted) {
        bcache_flush_all();
        set_clean(1);           /* everything out, then say so         */
    }
    mounted = 0;
    dev = 0;
    return 0;
}

static struct fs_type ext2_fs = {
    "ext2",
    ext2_mount,
    ext2_umount,
    ext2_open,
    ext2_unlink,
    ext2_rename,
    ext2_stat,
    ext2_readdir,
    ext2_statfs,
    ext2_sync,
    ext2_mkdir,
    ext2_rmdir,
    ext2_chdir,
    ext2_getcwd,
    ext2_readdir_in,
    ext2_dir_ino,
    ext2_dir_path_op,
    ext2_utime,
    ext2_futime,
    ext2_setattr,
    ext2_fsetattr,
    ext2_link,
    ext2_symlink,
    ext2_readlink,
    ext2_lstat,
    ext2_check,
    ext2_label,
    ext2_bmap,
    0
};

int ext2_init(void)
{
    return vfs_register(&ext2_fs);
}
