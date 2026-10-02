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
#include "console.h"
#include "pmm.h"
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
/* EXT2_SUPER_MAGIC is in uapi.h: statfs reports it, so a program needs
 * to be able to name it too. */
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
#define IN_UID_HIGH        120          /* Linux's osd2: uid and gid past */
#define IN_GID_HIGH        122          /* 65535, the high 16 bits each   */
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
#define EXT2_FT_FIFO         5
#define EXT2_FT_SYMLINK      7

/* ---------------------------------------------------------------- */
/* Mount state                                                       */
/* ---------------------------------------------------------------- */


/*
 * Whether the volume was in use when it was mounted -- read from the
 * superblock BEFORE mounting marks it in use, which is the only moment
 * the answer exists. Read afterwards it is always "dirty", and the
 * boot-time check then runs on every boot and reports every previous
 * run as unclean.
 */

/* ---------------------------------------------------------------- */
/* Block cache                                                       */
/*                                                                   */
/* Write-back, least-recently-used.  Sixteen blocks is 64 KB of a      */
/* 64 MB machine and is enough to keep a bitmap, an inode table block, */
/* an indirect block and a directory block all resident through one    */
/* operation, which is what stops a create from re-reading each of      */
/* them several times.                                                 */
/* ---------------------------------------------------------------- */

#define NBUF    64

struct bbuf {
    u8  data[EXT2_MAX_BLOCK];
    u32 blk;
    u32 stamp;                  /* for LRU                              */
    u8  valid;
    u8  dirty;
    u8  meta;                   /* dirty METADATA: in the running
                                 * transaction, so pinned -- it reaches
                                 * its home only after the journal has a
                                 * committed copy (see "The journal")   */
};

/*
 * ONE MOUNTED VOLUME. Everything this driver knows about a volume lives
 * here -- the device and where on it the filesystem starts, its
 * geometry and free counts, its block cache, its journal and running
 * transaction, and which of its inodes are open -- so that more than
 * one can be mounted (vfs.c's mount table). V is the volume being
 * worked on: vfs.c selects it (ext2_select) before every call, under
 * the filesystem lock, so nothing here can see two at once. The code
 * below still says `block_size` and `bcache`; the macros after this
 * make each of those V's.
 *
 * Shared by every volume, because they are scratch for the length of
 * one call: the journal's block buffers, symlink resolution's paths,
 * recovery's revoke table, and fsck's work maps.
 */
#define EXT2_MAX_OPEN   256

struct ext2_vol {
    struct blockdev *v_dev;
    int  v_mounted;
    u32  v_part_lba;              /* LBA the filesystem starts at         */
    u32  v_block_size, v_sectors_per_block;
    u32  v_inodes_count, v_blocks_count, v_r_blocks_count;
    u32  v_free_blocks, v_free_inodes;
    u32  v_first_data_block, v_blocks_per_group, v_inodes_per_group;
    u32  v_inode_size, v_first_ino, v_group_count;
    u32  v_gd_first_block;        /* block holding the descriptor table   */
    u32  v_addrs_per_block;       /* block_size / 4                       */
    u32  v_alloc_goal;            /* where the last allocation landed     */
    int  v_sb_dirty;
    int  v_was_unclean;           /* see ext2_mount_dev                   */
    char v_volume_label[17];
    struct bbuf v_bcache[NBUF];
    u32  v_bclock;
    u8   v_sbuf[EXT2_SUPER_OFF + 1024];
    /* the journal: see "The journal" */
    int  v_journal_on;
    u32  v_journal_forced, v_journal_commits, v_journal_replayed;
    u32  v_txn_started;           /* timer_jiffies() at its first change */
    int  v_txn_open, v_commit_due;
    u8 **v_txn_freed;             /* group -> page of bits, or 0          */
    u32  v_txn_freed_groups;
    u32 *v_jmap;                  /* journal block -> volume block        */
    u32  v_jmap_pages, v_j_maxlen, v_j_first, v_j_seq;
    u8   v_j_uuid[16];
    /* the inodes open on it, and how many times each */
    struct { u32 ino; int refs; } v_opened[EXT2_MAX_OPEN];
    /* where it is: see "Volumes" */
    int  idx;                   /* in vols[]; 0 is the root volume      */
    struct ext2_vol *parent;    /* the volume it is mounted on          */
    u32  mp;                    /* the directory it covers, a handle    */
    u32  st_dev;                /* what stat says; 0 for the root       */
    int  rdonly;
    u32  pages;                 /* how much of the page pool it took    */
};

static struct ext2_vol vol0;            /* the first mounted: the root */
static struct ext2_vol *V = &vol0;

/*
 * VOLUMES. The root volume is vol0; mount(2) adds the others, each on a
 * directory of one already mounted -- so all of them are this driver's,
 * and crossing from one to another is part of walking a path (lookup_x)
 * rather than something vfs.c has to know about.
 *
 * Outside this file an inode is named by a HANDLE: the volume's index in
 * the top byte, the inode number below. The root volume's index is 0, so
 * its handles are plain inode numbers -- everything that held one before
 * there were mounts still holds the right thing. sel() makes a handle's
 * volume the current one and gives back the inode number; hnd() goes
 * the other way.
 *
 * WHICH VOLUME IS CURRENT is a side effect of the walk: path_walk leaves
 * V at the volume its answer is on. Every entry point therefore starts
 * from a handle or a path, never from whatever V the last call left --
 * that volume may since have been unmounted.
 */
#define EXT2_MAX_VOLS   8
#define VOL_SHIFT       24
#define INO_MASK        0x00ffffffUL

static struct ext2_vol *vols[EXT2_MAX_VOLS] = { &vol0 };

static u32 sel(u32 h)
{
    struct ext2_vol *v = vols[(h >> VOL_SHIFT) % EXT2_MAX_VOLS];

    V = v ? v : &vol0;
    return h & INO_MASK;
}

static u32 hnd(u32 ino)
{
    return ((u32)V->idx << VOL_SHIFT) | ino;
}

#define dev               (V->v_dev)
#define mounted           (V->v_mounted)
#define part_lba          (V->v_part_lba)
#define block_size        (V->v_block_size)
#define sectors_per_block (V->v_sectors_per_block)
#define inodes_count      (V->v_inodes_count)
#define blocks_count      (V->v_blocks_count)
#define r_blocks_count    (V->v_r_blocks_count)
#define free_blocks       (V->v_free_blocks)
#define free_inodes       (V->v_free_inodes)
#define first_data_block  (V->v_first_data_block)
#define blocks_per_group  (V->v_blocks_per_group)
#define inodes_per_group  (V->v_inodes_per_group)
#define inode_size        (V->v_inode_size)
#define first_ino         (V->v_first_ino)
#define group_count       (V->v_group_count)
#define gd_first_block    (V->v_gd_first_block)
#define addrs_per_block   (V->v_addrs_per_block)
#define alloc_goal        (V->v_alloc_goal)
#define sb_dirty          (V->v_sb_dirty)
#define was_unclean       (V->v_was_unclean)
#define volume_label      (V->v_volume_label)
#define bcache            (V->v_bcache)
#define bclock            (V->v_bclock)
#define sbuf              (V->v_sbuf)
#define journal_on        (V->v_journal_on)
#define journal_forced    (V->v_journal_forced)
#define journal_commits   (V->v_journal_commits)
#define journal_replayed  (V->v_journal_replayed)
#define txn_started       (V->v_txn_started)
#define txn_open          (V->v_txn_open)
#define commit_due        (V->v_commit_due)
#define txn_freed         (V->v_txn_freed)
#define txn_freed_groups  (V->v_txn_freed_groups)
#define jmap              (V->v_jmap)
#define jmap_pages        (V->v_jmap_pages)
#define j_maxlen          (V->v_j_maxlen)
#define j_first           (V->v_j_first)
#define j_seq             (V->v_j_seq)
#define j_uuid            (V->v_j_uuid)
#define opened            (V->v_opened)


/* The journal, below; the cache has to know whether one is in use. */
static int journal_commit(void);

/* The superblock lives at byte 1024, which is inside block 0 when the
 * block size is 2048 or 4096 and is block 1 when it is 1024.  It is read
 * and written through its own buffer rather than the cache, so that
 * nothing can evict it half-written. */

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
/*
 * The buffer to reuse: an empty one, or the least recently used --
 * written back first if it is dirty. A pinned buffer (dirty metadata in
 * the running transaction) is never chosen: writing it home before its
 * transaction commits is exactly what the journal exists to prevent. If
 * every buffer is pinned, the transaction is committed here, in the
 * middle of a call; that keeps the volume consistent at each commit but
 * splits one call across two transactions, so it is counted.
 */
static struct bbuf *pick_victim(void)
{
    struct bbuf *victim = 0;
    int i, pass;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < NBUF; i++) {
            struct bbuf *b = &bcache[i];

            if (!b->valid) {
                victim = b;
                break;
            }
            if (b->dirty && b->meta) {
                continue;
            }
            if (!victim || b->stamp < victim->stamp) {
                victim = b;
            }
        }
        if (victim) {
            break;
        }
        journal_forced++;
        if (journal_commit() != 0) {
            return 0;
        }
    }
    if (!victim || bflush(victim) != 0) {
        return 0;
    }
    return victim;
}

static struct bbuf *bget(u32 blk)
{
    struct bbuf *victim;
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
    victim = pick_victim();
    if (!victim) {
        return 0;
    }
    if (bread_raw(blk, victim->data) != 0) {
        victim->valid = 0;
        return 0;
    }
    victim->blk = blk;
    victim->valid = 1;
    victim->dirty = 0;
    victim->meta = 0;
    victim->stamp = ++bclock;
    return victim;
}

/*
 * A buffer for a block that is about to be overwritten completely --
 * a freshly allocated one.  Skips the read, which is the whole point.
 */
/*
 * Dirty as DATA, not metadata: written home whenever it is evicted, and
 * in any case before the transaction that points at it commits -- ext3's
 * "ordered" mode. A freshly zeroed block starts this way; one that turns
 * out to be an indirect or directory block becomes metadata the moment
 * bdirty() is called on it.
 */
static struct bbuf *bget_zero(u32 blk)
{
    struct bbuf *victim = 0;
    int i;

    if (blk >= blocks_count) {
        return 0;
    }
    for (i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].blk == blk) {
            victim = &bcache[i];
            break;
        }
    }
    if (!victim) {
        victim = pick_victim();
        if (!victim) {
            return 0;
        }
        victim->meta = 0;
    }
    memset(victim->data, 0, block_size);
    victim->blk = blk;
    victim->valid = 1;
    victim->dirty = 1;
    victim->stamp = ++bclock;
    return victim;
}


/* Dirty metadata: a bitmap, an inode table block, a group descriptor, an
 * indirect or a directory block. With a journal it joins the running
 * transaction and stays in the cache until that commits. */
static void bdirty(struct bbuf *b)
{
    b->dirty = 1;
    if (journal_on) {
        b->meta = 1;
        if (!txn_open) {
            txn_open = 1;
            txn_started = timer_jiffies();
        }
    }
}

/* Dirty file contents: see bget_zero. */
static void bdirty_data(struct bbuf *b)
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
            bcache[i].meta = 0;
        }
    }
}

static void bcache_reset(void)
{
    int i;

    for (i = 0; i < NBUF; i++) {
        bcache[i].valid = 0;
        bcache[i].dirty = 0;
        bcache[i].meta = 0;
    }
    bclock = 0;
}

/* Every dirty buffer home, metadata included: without a journal this is
 * the whole of sync, and with one it is the checkpoint after a commit. */
static int bcache_flush_raw(void)
{
    int i, first = 0, err;

    for (i = 0; i < NBUF; i++) {
        err = bflush(&bcache[i]);
        if (err != 0 && first == 0) {
            first = err;
        }
        bcache[i].meta = 0;
    }
    return first;
}

/* Everything onto the disk: through the journal when there is one. */
static int bcache_flush_all(void)
{
    if (journal_on) {
        return journal_commit();
    }
    return bcache_flush_raw();
}

/*
 * At the end of a call that changed the namespace or an inode -- close,
 * unlink, mkdir, rmdir, link, symlink, rename, utime, chmod and chown.
 * The driver has always made those durable before the call returns, and
 * programs and tests rely on it: a file closed is a file on the disk.
 * Without a journal everything goes out here. With one, the call asks
 * for a commit, which happens as the call ends (ext2_boundary) -- the
 * same promise, and atomic now. Plain writes to a file still open
 * commit within five seconds, or at sync and fsync.
 */
static int sb_write(void);
static int op_flush(void)
{
    int a, b;

    if (journal_on) {
        commit_due = 1;
        return 0;
    }
    a = bcache_flush_raw();
    b = sb_write();
    return a != 0 ? a : b;
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
 * BLOCKS FREED IN THE RUNNING TRANSACTION ARE NOT GIVEN OUT AGAIN until
 * it commits. File data is written home before the commit (ordered
 * mode), so a block freed by an unlink and handed straight to a new file
 * would take the new file's bytes -- and if the machine stopped before
 * the commit, the journal would bring the unlinked file back, pointing
 * at them. One bit per block, per group, in a page allocated the first
 * time a group frees something; cleared at every commit.
 */

static int freed_in_txn(u32 g, u32 bit)
{
    return txn_freed && g < txn_freed_groups && txn_freed[g] &&
           bitmap_test(txn_freed[g], bit);
}

static void note_freed(u32 g, u32 bit)
{
    if (!journal_on || !txn_freed || g >= txn_freed_groups) {
        return;
    }
    if (!txn_freed[g]) {
        u32 pa = pmm_alloc();

        if (!pa) {
            return;     /* no memory: the one case reuse is allowed */
        }
        txn_freed[g] = (u8 *)pa;
        memset(txn_freed[g], 0, PAGE_SIZE);
    }
    bitmap_set(txn_freed[g], bit);
}

static void txn_freed_clear(void)
{
    u32 g;

    for (g = 0; txn_freed && g < txn_freed_groups; g++) {
        if (txn_freed[g]) {
            pmm_free((u32)txn_freed[g]);
            txn_freed[g] = 0;
        }
    }
}

/* The first bit in [start, limit) free in the bitmap AND not freed in the
 * running transaction, or limit. */
static u32 alloc_first_free(const u8 *map, u32 g, u32 start, u32 limit)
{
    u32 bit = bitmap_first_free(map, start, limit);

    while (bit < limit && freed_in_txn(g, bit)) {
        bit = bitmap_first_free(map, bit + 1, limit);
    }
    return bit;
}

/*
 * Allocate one block, preferring one near `goal`.  Returns 0 on failure,
 * which is a safe sentinel: block 0 is never allocatable.
 */
static u32 block_alloc_once(u32 goal);

static u32 block_alloc(u32 goal)
{
    u32 blk = block_alloc_once(goal);

    /* Only blocks this transaction freed are left: commit it, which
     * makes them anybody's, and try again. */
    if (blk == 0 && free_blocks != 0 && journal_on && txn_open) {
        journal_forced++;
        if (journal_commit() == 0) {
            blk = block_alloc_once(goal);
        }
    }
    return blk;
}

static u32 block_alloc_once(u32 goal)
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
        bit = alloc_first_free(map, g, i, limit);
        if (bit == limit && i != 0) {
            bit = alloc_first_free(map, g, 0, i);
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
    note_freed(g, bit);
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
    u32 uid;                    /* 32 bits: the high halves are at 120 */
    u32 gid;
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
    ei->uid = le16(raw + IN_UID) | ((u32)le16(raw + IN_UID_HIGH) << 16);
    ei->gid = le16(raw + IN_GID) | ((u32)le16(raw + IN_GID_HIGH) << 16);
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
    put_le16(raw + IN_UID, (u16)ei->uid);
    put_le16(raw + IN_GID, (u16)ei->gid);
    put_le16(raw + IN_UID_HIGH, (u16)(ei->uid >> 16));
    put_le16(raw + IN_GID_HIGH, (u16)(ei->gid >> 16));
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
        bdirty_data(b);
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
                    bdirty_data(b);
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

/* The start of every call: the root volume, until a handle or a walk
 * says otherwise. Whether anything is mounted at all. */
static int enter(void)
{
    V = &vol0;
    return vol0.v_mounted;
}

/* The handle of this task's "/": a chroot's, or the real one. */
static u32 root_handle(void)
{
    u32 r = vfs_root_ino();

    return (r == 0) ? EXT2_ROOT_INO : r;
}

static u32 cwd_handle(void)
{
    u32 c = vfs_cwd_ino();

    return (c == 0) ? root_handle() : c;
}

/* Start a walk at the root or the working directory: make its volume
 * current and give back its inode number there. */
static u32 go_root(void)
{
    return sel(root_handle());
}

static u32 go_cwd(void)
{
    return sel(cwd_handle());
}

/* Is inode `d` of the current volume this task's "/"? */
static int at_root(u32 d)
{
    return hnd(d) == root_handle();
}

/* The volume mounted on inode `d` of the current one, if any. */
static struct ext2_vol *mounted_on(u32 d)
{
    u32 h = hnd(d);
    int i;

    for (i = 1; i < EXT2_MAX_VOLS; i++) {
        if (vols[i] && vols[i]->parent == V && vols[i]->mp == h) {
            return vols[i];
        }
    }
    return 0;
}

/* Does inode `d` of the current volume have anything mounted on it? */
static int is_mountpoint(u32 d)
{
    return mounted_on(d) != 0;
}

/*
 * dir_lookup, CROSSING MOUNTS: a name that is a mount point gives the
 * mounted volume's root, and ".." at a volume's root gives the parent
 * of the directory it is mounted on. What a walk uses, and anything
 * that wants the thing a whole path names. What it must NOT be used for
 * is a lookup that is about to change the entry -- unlink, rmdir,
 * rename -- which wants the name in this directory as it stands, and
 * says EBUSY if that is a mount point.
 *
 * V changes only on success.
 */
static int dir_lookup(u32 dino, const char *name, u32 nlen, u32 *ino,
                      u8 *type);

static int lookup_x(u32 d, const char *name, u32 nlen, u32 *ino, u8 *type)
{
    struct ext2_vol *was = V, *m;
    int err;

    if (nlen == 2 && name[0] == '.' && name[1] == '.' &&
        d == EXT2_ROOT_INO && V->parent && !at_root(d)) {
        d = sel(V->mp);         /* up to the directory it covers      */
        err = dir_lookup(d, name, nlen, ino, type);
        if (err != 0) {
            V = was;
        }
        return err;
    }
    err = dir_lookup(d, name, nlen, ino, type);
    if (err != 0) {
        return err;
    }
    while ((m = mounted_on(*ino)) != 0) {
        V = m;
        *ino = EXT2_ROOT_INO;
        if (type) {
            *type = EXT2_FT_DIR;
        }
    }
    return 0;
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

/*
 * Is this a fast symlink -- one whose "block pointers" are its target's
 * bytes? Anything that walks or frees an inode's blocks must ask first:
 * the check once read "gcc" as block 0x00636367, found it past the end
 * of the volume and cut it, emptying the link; and freeing a deleted
 * link's blocks would have freed block 97 for a link to "a". i_blocks
 * is 0 for a fast link and for nothing else that is a link, which is how
 * Linux's ext2 tells them apart.
 */
static int fast_symlink(const struct einode *ei)
{
    return S_ISLNK(ei->mode) && ei->blocks == 0;
}

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
        d = go_root();
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
        if (at_root(d) && strcmp(comp, "..") == 0) {
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
        err = lookup_x(d, comp, n, &ino, &type);
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
                d = go_root();
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

    err = path_walk(go_cwd(), path, &d, name, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || name[0] == '\0') {
        *ino = d;
        return 0;
    }
    return lookup_x(d, name, name_len_of(name), ino, 0);
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

        err = path_walk(go_cwd(), cur, &d, name, &last_is_dir);
        if (err != 0) {
            return err;
        }
        if (last_is_dir || name[0] == '\0') {
            *ino = d;
            return 0;
        }
        err = lookup_x(d, name, name_len_of(name), &got, 0);
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
    int depth = 0;

    buf[end] = '\0';
    if (at_root(cur)) {
        if (size < 2) {
            return -ERANGE;
        }
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    while (!at_root(cur)) {
        u32 parent, n;
        int i, found = 0;
        char name[NAME_MAX + 1];
        u8 type;
        int err;

        if (++depth > 64) {
            return -ELOOP;
        }
        /* The root of a mounted volume is named by the directory it
         * covers: carry on from there, on the volume below. */
        if (cur == EXT2_ROOT_INO && V->parent) {
            cur = sel(V->mp);
            continue;
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

/* ---------------------------------------------------------------- */
/* The journal                                                       */
/*                                                                   */
/* ext3's: ext2 plus a log in an inode (number 8), in the JBD format   */
/* e2fsprogs and Linux read -- which is what lets a test pull the plug */
/* on the machine and have the HOST's e2fsck replay what this wrote.   */
/* ---------------------------------------------------------------- */

/*
 * HOW A CHANGE REACHES THE DISK. Metadata dirtied by a call -- bitmaps,
 * inodes, group descriptors, indirect and directory blocks -- collects
 * in the cache as the running transaction, and stays there: a pinned
 * buffer is never evicted. A commit then
 *
 *   1. writes file data home (ordered mode: nothing the transaction
 *      points at can be older on the disk than the pointer),
 *   2. writes a descriptor block and a copy of every pinned block into
 *      the log, then the journal superblock saying where they start,
 *      then a commit block -- the transaction exists from that moment,
 *   3. writes every pinned block home (the checkpoint), and
 *   4. marks the log empty.
 *
 * A stop anywhere before the commit block leaves the old volume; after
 * it, mount (or e2fsck) replays the copies over whatever half-finished
 * checkpoint it finds. Either way the volume is one the calls made, not
 * something between two of them.
 *
 * WHEN. A commit happens only between calls -- vfs.c tells the
 * filesystem when its lock is let go (ext2_boundary) -- once the
 * transaction is five seconds old or half the cache is pinned; at sync,
 * fsync and unmount; and, failing all of those, when a call has pinned
 * the whole cache (journal_forced counts that).
 *
 * WHAT IS LEFT OUT, deliberately: the log is emptied at every commit,
 * so a block is never in two transactions and nothing here writes a
 * revoke record. Replay still honours revokes, so a journal Linux wrote
 * replays correctly. No checksums and no 64-bit block numbers: a
 * journal asking for either is not used (and if it needs replaying, the
 * volume is not mounted).
 */

#define FEAT_COMPAT_HAS_JOURNAL   0x0004
#define FEAT_INCOMPAT_RECOVER     0x0004
#define SB_JOURNAL_UUID           208
#define SB_JOURNAL_INUM           224
#define SB_JOURNAL_DEV            228

#define JBD_MAGIC                 0xC03B3998UL
#define JBD_DESCRIPTOR            1
#define JBD_COMMIT                2
#define JBD_SB_V1                 3
#define JBD_SB_V2                 4
#define JBD_REVOKE                5

#define JBD_FLAG_ESCAPE           1
#define JBD_FLAG_SAME_UUID        2
#define JBD_FLAG_LAST_TAG         8

/* Journal superblock fields, BIG-endian, unlike everything else here. */
#define JS_BLOCKSIZE              12
#define JS_MAXLEN                 16
#define JS_FIRST                  20
#define JS_SEQUENCE               24
#define JS_START                  28
#define JS_FEATURE_INCOMPAT       40
#define JS_UUID                   48

#define JBD_INCOMPAT_REVOKE       0x01
#define JBD_INCOMPAT_ASYNC_COMMIT 0x04
#define JBD_INCOMPAT_OK           (JBD_INCOMPAT_REVOKE | JBD_INCOMPAT_ASYNC_COMMIT)

#define JBD_HEADER                12      /* magic, blocktype, sequence  */
#define JBD_TAG_BYTES             8       /* blocknr, checksum, flags    */

static u8   jbuf[EXT2_MAX_BLOCK];
static u8   jbuf2[EXT2_MAX_BLOCK];
static u8   jsbimg[EXT2_MAX_BLOCK];

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u16 be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void put_be16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

/* The log is circular from j_first to j_maxlen - 1. */
static u32 jnext(u32 pos)
{
    pos++;
    return pos >= j_maxlen ? j_first : pos;
}

static int jread(u32 pos, u8 *data)
{
    return pos < j_maxlen ? bread_raw(jmap[pos], data) : -EIO;
}

static int jwrite(u32 pos, const u8 *data)
{
    return pos < j_maxlen ? bwrite_raw(jmap[pos], data) : -EIO;
}

static void jheader(u8 *b, u32 type, u32 seq)
{
    memset(b, 0, block_size);
    put_be32(b, JBD_MAGIC);
    put_be32(b + 4, type);
    put_be32(b + 8, seq);
}

/* The journal superblock's start and sequence: where the log begins (0
 * when it is empty) and the transaction expected there. */
static int jsb_set(u32 start, u32 seq)
{
    int err = jread(0, jbuf);

    if (err != 0) {
        return err;
    }
    put_be32(jbuf + JS_START, start);
    put_be32(jbuf + JS_SEQUENCE, seq);
    return jwrite(0, jbuf);
}

/* The volume block holding the superblock, whole: block 1 when blocks
 * are 1 KB, block 0 (with the boot sector in front of it) otherwise. */
static u32 sb_block(void)
{
    return block_size == 1024 ? 1 : 0;
}

/* That block as it should read now: the disk's, with sbuf laid over the
 * superblock's 1024 bytes. */
static int sb_block_image(u8 *out)
{
    u32 blk = sb_block();
    u32 off = EXT2_SUPER_OFF - blk * block_size;
    int err = bread_raw(blk, out);

    if (err != 0) {
        return err;
    }
    memcpy(out + off, sbuf + EXT2_SUPER_OFF, 1024);
    return 0;
}

/* Copy a block into the log, escaping it if it happens to begin with the
 * journal's magic number -- replay would take it for a log block. */
static int jlog_block(u32 pos, const u8 *data, u16 *flags)
{
    memcpy(jbuf2, data, block_size);
    if (be32(jbuf2) == JBD_MAGIC) {
        put_be32(jbuf2, 0);
        *flags |= JBD_FLAG_ESCAPE;
    }
    return jwrite(pos, jbuf2);
}

/*
 * A journal whose superblock is damaged, made afresh in place -- what
 * tune2fs -j would leave, in the same blocks: version 2, no features,
 * the log empty, the volume's UUID. Its blocks are the inode's; if the
 * inode itself is damaged there is nothing to make a journal in.
 */
static int journal_reinit(void)
{
    u8 *s = sbuf + EXT2_SUPER_OFF;
    u32 jino = le32(s + SB_JOURNAL_INUM), blk, maxlen;
    struct einode ei;

    if (jino == 0 || iread(jino, &ei) != 0) {
        return -EIO;
    }
    maxlen = ei.size / block_size;
    if (maxlen < NBUF + 16 || bmap(&ei, 0, 0, &blk) != 0 || blk == 0) {
        return -EIO;
    }
    memset(jbuf, 0, block_size);
    put_be32(jbuf, JBD_MAGIC);
    put_be32(jbuf + 4, JBD_SB_V2);
    put_be32(jbuf + JS_BLOCKSIZE, block_size);
    put_be32(jbuf + JS_MAXLEN, maxlen);
    put_be32(jbuf + JS_FIRST, 1);
    put_be32(jbuf + JS_SEQUENCE, 1);
    put_be32(jbuf + JS_START, 0);
    memcpy(jbuf + JS_UUID, s + SB_UUID, 16);
    put_be32(jbuf + 64, 1);                     /* s_nr_users */
    return bwrite_raw(blk, jbuf);
}

/* kstat's view, and its test knob (uapi.h). */
static u32 jstop_how, jstop_left;
static int committing_for_sync;

void ext2_journal_stats(struct journalstats *js)
{
    /* The root volume's. Not under the filesystem lock, so it must not
     * move V: another task may be asleep in the middle of a call. */
    js->on = vol0.v_journal_on;
    js->commits = vol0.v_journal_commits;
    js->forced = vol0.v_journal_forced;
    js->replayed = vol0.v_journal_replayed;
}

void ext2_journal_stop(u32 how, u32 n)
{
    jstop_how = how;
    jstop_left = n;
}

/* The power cut, if one was asked for at this point of this commit. */
static void jstop_check(u32 how)
{
    if ((jstop_how & 0xff) == how && jstop_left &&
        (!(jstop_how & JSTOP_SYNC_ONLY) || committing_for_sync) &&
        --jstop_left == 0) {
        kputs(how == JSTOP_COMMITTED ? "journal: stopped after the commit block\n"
                                     : "journal: stopped before the commit block\n");
        halt();
    }
}

static int journal_commit(void)
{
    int idx[NBUF];
    u32 n = 0, pos, tag, i, seq;
    int with_sb, err;
    u16 flags;

    if (!journal_on) {
        return bcache_flush_raw();
    }
    for (i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].dirty && bcache[i].meta) {
            idx[n++] = (int)i;
        }
    }
    with_sb = sb_dirty;

    /* 1. File data home first: ordered mode. */
    for (i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].dirty && !bcache[i].meta) {
            err = bflush(&bcache[i]);
            if (err != 0) {
                return err;
            }
        }
    }
    commit_due = 0;
    if (n == 0 && !with_sb) {
        txn_open = 0;
        txn_freed_clear();
        return 0;
    }

    /* 2. The log: descriptor, copies, then where it is, then commit. */
    seq = j_seq;
    jheader(jbuf, JBD_DESCRIPTOR, seq);
    tag = JBD_HEADER;
    pos = jnext(j_first);
    for (i = 0; i < n + (u32)(with_sb ? 1 : 0); i++) {
        struct bbuf *b = (i < n) ? &bcache[idx[i]] : 0;
        u32 blk = b ? b->blk : sb_block();

        flags = (i == 0) ? 0 : JBD_FLAG_SAME_UUID;
        if (b) {
            err = jlog_block(pos, b->data, &flags);
        } else {
            err = sb_block_image(jsbimg);
            if (err == 0) {
                err = jlog_block(pos, jsbimg, &flags);
            }
        }
        if (err != 0) {
            return err;
        }
        if (i + 1 == n + (u32)(with_sb ? 1 : 0)) {
            flags |= JBD_FLAG_LAST_TAG;
        }
        put_be32(jbuf + tag, blk);
        put_be16(jbuf + tag + 4, 0);            /* no checksum */
        put_be16(jbuf + tag + 6, flags);
        tag += JBD_TAG_BYTES;
        if (i == 0) {
            memcpy(jbuf + tag, j_uuid, 16);
            tag += 16;
        }
        pos = jnext(pos);
    }
    err = jwrite(j_first, jbuf);
    if (err == 0) {
        err = jsb_set(j_first, seq);
    }
    if (err == 0) {
        jstop_check(JSTOP_UNCOMMITTED);
        jheader(jbuf, JBD_COMMIT, seq);
        put_be32(jbuf + 48 + 4, now_secs());    /* h_commit_sec, low half */
        err = jwrite(pos, jbuf);
    }
    if (err != 0) {
        return err;
    }
    jstop_check(JSTOP_COMMITTED);

    /* 3. The checkpoint: every copy home. */
    err = bcache_flush_raw();
    if (err == 0 && with_sb) {
        err = sb_write();
    }
    if (err != 0) {
        return err;
    }

    /* 4. Empty again. */
    j_seq = seq + 1;
    err = jsb_set(0, j_seq);
    txn_open = 0;
    txn_freed_clear();
    journal_commits++;
    return err;
}

/* --- replay --- */

/*
 * Revoke records: a block a later transaction freed, which an earlier
 * one's copy must not be replayed over. Kept as (block, sequence) pairs
 * in pages for the length of one recovery.
 */
#define REVOKE_PER_PAGE   (PAGE_SIZE / 8)
#define REVOKE_PAGES      16
static u32 *revoke_page[REVOKE_PAGES];
static u32  revoke_n;

static void revoke_add(u32 blk, u32 seq)
{
    u32 i, p;

    for (i = 0; i < revoke_n; i++) {
        u32 *e = revoke_page[i / REVOKE_PER_PAGE] + 2 * (i % REVOKE_PER_PAGE);

        if (e[0] == blk) {
            if ((s32)(seq - e[1]) > 0) {
                e[1] = seq;
            }
            return;
        }
    }
    p = revoke_n / REVOKE_PER_PAGE;
    if (p >= REVOKE_PAGES) {
        return;
    }
    if (!revoke_page[p]) {
        u32 pa = pmm_alloc();

        if (!pa) {
            return;
        }
        revoke_page[p] = (u32 *)pa;
    }
    revoke_page[p][2 * (revoke_n % REVOKE_PER_PAGE)] = blk;
    revoke_page[p][2 * (revoke_n % REVOKE_PER_PAGE) + 1] = seq;
    revoke_n++;
}

/* Is this block revoked by the transaction `seq` or a later one? */
static int revoked(u32 blk, u32 seq)
{
    u32 i;

    for (i = 0; i < revoke_n; i++) {
        u32 *e = revoke_page[i / REVOKE_PER_PAGE] + 2 * (i % REVOKE_PER_PAGE);

        if (e[0] == blk && (s32)(e[1] - seq) >= 0) {
            return 1;
        }
    }
    return 0;
}

static void revoke_free(void)
{
    u32 i;

    for (i = 0; i < REVOKE_PAGES; i++) {
        if (revoke_page[i]) {
            pmm_free((u32)revoke_page[i]);
            revoke_page[i] = 0;
        }
    }
    revoke_n = 0;
}

/*
 * One pass over the log from `start`, transaction `seq` on. Pass 0 finds
 * where the committed transactions end and gathers the revokes; pass 1
 * writes every copy home that is not revoked. Returns the sequence
 * number of the first transaction with no commit block -- the end.
 */
static u32 journal_pass(u32 start, u32 seq, u32 end, int replay, int *err)
{
    u32 pos = start;
    u32 steps = 0;

    *err = 0;
    while (replay ? (s32)(seq - end) < 0 : 1) {
        if (steps++ > j_maxlen) {
            break;                  /* a log that loops: stop          */
        }
        if (jread(pos, jbuf) != 0) {
            *err = -EIO;
            break;
        }
        if (be32(jbuf) != JBD_MAGIC || be32(jbuf + 8) != seq) {
            break;
        }
        switch (be32(jbuf + 4)) {
        case JBD_DESCRIPTOR: {
            u32 t = JBD_HEADER;

            pos = jnext(pos);
            for (;;) {
                u32 blk, fl;

                if (t + JBD_TAG_BYTES > block_size) {
                    break;
                }
                blk = be32(jbuf + t);
                fl = be16(jbuf + t + 6);
                t += JBD_TAG_BYTES;
                if (!(fl & JBD_FLAG_SAME_UUID)) {
                    t += 16;
                }
                if (replay && !revoked(blk, seq) && blk < blocks_count) {
                    /* jbuf holds the descriptor; the copy goes through
                     * jbuf2 on its way home. */
                    if (jread(pos, jbuf2) != 0) {
                        *err = -EIO;
                        return seq;
                    }
                    if (fl & JBD_FLAG_ESCAPE) {
                        put_be32(jbuf2, JBD_MAGIC);
                    }
                    if (bwrite_raw(blk, jbuf2) != 0) {
                        *err = -EIO;
                        return seq;
                    }
                }
                pos = jnext(pos);
                if (fl & JBD_FLAG_LAST_TAG) {
                    break;
                }
            }
            continue;
        }
        case JBD_COMMIT:
            seq++;
            pos = jnext(pos);
            continue;
        case JBD_REVOKE:
            if (!replay) {
                u32 used = be32(jbuf + JBD_HEADER), r;

                for (r = JBD_HEADER + 4; r + 4 <= used && r + 4 <= block_size;
                     r += 4) {
                    revoke_add(be32(jbuf + r), seq);
                }
            }
            pos = jnext(pos);
            continue;
        default:
            break;
        }
        break;
    }
    return seq;
}

/* Replay whatever the log holds. Returns how many transactions. */
static int journal_recover(u32 *replayed)
{
    u32 start, seq, end;
    int err;

    *replayed = 0;
    if (jread(0, jbuf) != 0) {
        return -EIO;
    }
    start = be32(jbuf + JS_START);
    seq = be32(jbuf + JS_SEQUENCE);
    if (start == 0) {
        j_seq = seq;
        return 0;
    }
    revoke_free();
    end = journal_pass(start, seq, 0, 0, &err);
    if (err == 0 && end != seq) {
        journal_pass(start, seq, end, 1, &err);
    }
    revoke_free();
    if (err != 0) {
        return err;
    }
    *replayed = end - seq;
    j_seq = end;
    return jsb_set(0, end);
}

/*
 * Find the journal and make it usable: map its blocks, check its
 * superblock, replay it if it holds anything. Returns 0 with journal_on
 * set or clear, or -errno to refuse the mount (a log that needs
 * replaying and cannot be).
 */
static int journal_setup(u32 *replayed)
{
    u8 *s = sbuf + EXT2_SUPER_OFF;
    u32 compat = le32(s + SB_FEATURE_COMPAT);
    u32 incompat = le32(s + SB_FEATURE_INCOMPAT);
    u32 jino = le32(s + SB_JOURNAL_INUM);
    int needs = (incompat & FEAT_INCOMPAT_RECOVER) != 0;
    struct einode ei;
    u32 i, pages, jsb_blk;
    int err;

    *replayed = 0;
    journal_on = 0;
    if (!(compat & FEAT_COMPAT_HAS_JOURNAL) || jino == 0) {
        return needs ? -EOPNOTSUPP : 0;     /* an external journal */
    }
    err = iread(jino, &ei);
    if (err != 0) {
        return -EIO;
    }
    if (bmap(&ei, 0, 0, &jsb_blk) != 0 || jsb_blk == 0 ||
        bread_raw(jsb_blk, jbuf) != 0) {
        return -EIO;
    }
    if (be32(jbuf) != JBD_MAGIC ||
        (be32(jbuf + 4) != JBD_SB_V1 && be32(jbuf + 4) != JBD_SB_V2) ||
        be32(jbuf + JS_BLOCKSIZE) != block_size) {
        return -EINVAL;
    }
    if (be32(jbuf + 4) == JBD_SB_V2 &&
        (be32(jbuf + JS_FEATURE_INCOMPAT) & ~(u32)JBD_INCOMPAT_OK) != 0) {
        return -EOPNOTSUPP;
    }
    j_maxlen = be32(jbuf + JS_MAXLEN);
    j_first = be32(jbuf + JS_FIRST);
    if (j_maxlen > ei.size / block_size || j_first == 0 ||
        j_first + NBUF + 8 > j_maxlen) {
        return -EINVAL;
    }
    if (be32(jbuf + 4) == JBD_SB_V2) {
        memcpy(j_uuid, jbuf + JS_UUID, 16);
    } else {
        memset(j_uuid, 0, 16);
    }

    /* The whole map, once: the log is written a block at a time from
     * inside a commit, where walking indirect blocks through the cache
     * is the last thing wanted. */
    pages = (j_maxlen * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (jmap && jmap_pages < pages) {
        pmm_free_pages((u32)jmap, jmap_pages);
        jmap = 0;
    }
    if (!jmap) {
        u32 pa = pmm_alloc_pages(pages);

        if (!pa) {
            return -ENOMEM;
        }
        jmap = (u32 *)pa;
        jmap_pages = pages;
    }
    for (i = 0; i < j_maxlen; i++) {
        if (bmap(&ei, i, 0, &jmap[i]) != 0 || jmap[i] == 0) {
            return -EIO;
        }
    }

    err = journal_recover(replayed);
    if (err != 0) {
        return err;
    }
    if (!txn_freed) {
        u32 pa = pmm_alloc();

        if (pa) {
            txn_freed = (u8 **)pa;
            memset(txn_freed, 0, PAGE_SIZE);
            txn_freed_groups = PAGE_SIZE / sizeof(u8 *);
        }
    }
    journal_on = 1;
    txn_open = 0;
    return 0;
}

/* From vfs.c, as its filesystem lock is let go: between two calls, the
 * only place a commit leaves nothing half-done. */
static void boundary_one(void)
{
    u32 pinned = 0, i;

    if (!mounted || !journal_on || (!txn_open && !commit_due)) {
        return;
    }
    /* A test has armed a stop for the next sync's commit: nothing may
     * commit before it, or the change it is waiting for goes into some
     * other transaction and the sync finds nothing to do. */
    if (jstop_left && (jstop_how & JSTOP_SYNC_ONLY)) {
        return;
    }
    for (i = 0; i < NBUF; i++) {
        if (bcache[i].valid && bcache[i].dirty && bcache[i].meta) {
            pinned++;
        }
    }
    if (commit_due || pinned >= NBUF / 2 ||
        (u32)(timer_jiffies() - txn_started) >= 5 * HZ) {
        journal_commit();
    }
}

static void ext2_boundary(void)
{
    int i;

    for (i = 0; i < EXT2_MAX_VOLS; i++) {
        if (vols[i]) {
            V = vols[i];
            boundary_one();
        }
    }
    V = &vol0;
}

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
    /* RECOVER (needs_recovery) is a journal with something in it;
     * journal_setup replays it or refuses the mount. */
    if ((incompat & ~(u32)(FEAT_INCOMPAT_OK | FEAT_INCOMPAT_RECOVER)) != 0) {
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

    {
        u32 replayed;
        int err = journal_setup(&replayed);

        if (err == -EINVAL || err == -EIO) {
            /* Damaged. Made afresh, it can be used from here on -- but
             * whatever it held is gone, so if it held anything the
             * volume is checked, as after an unclean stop. */
            if (journal_reinit() == 0 && journal_setup(&replayed) == 0) {
                kputs("ext2: the journal was damaged and has been made afresh\n");
                if (incompat & FEAT_INCOMPAT_RECOVER) {
                    was_unclean = 1;
                    incompat &= ~(u32)FEAT_INCOMPAT_RECOVER;
                }
                err = 0;
            }
        }
        if (err != 0 && !(incompat & FEAT_INCOMPAT_RECOVER)) {
            /* A journal this driver cannot use, holding nothing: the
             * volume is used without it, as an ext2 driver would. */
            kputs("ext2: the journal is not usable; running without it\n");
            journal_on = 0;
            err = 0;
        }
        if (err != 0) {
            /*
             * The volume says its journal holds changes and the journal
             * cannot be read -- damaged, or a kind this driver cannot
             * replay. Linux refuses such a volume; refusing the ROOT
             * volume would leave a machine that cannot start. Instead it
             * is mounted without the journal, and the full check runs
             * as it would after any unclean stop: what the journal held
             * is lost, the volume is made consistent.
             */
            kputs("ext2: the journal needs replaying and cannot be "
                  "used; checking the volume instead\n");
            journal_on = 0;
            was_unclean = 1;
            put_le32(sbp(SB_FEATURE_INCOMPAT),
                     le32(s + SB_FEATURE_INCOMPAT) & ~(u32)FEAT_INCOMPAT_RECOVER);
            incompat &= ~(u32)FEAT_INCOMPAT_RECOVER;
        }
        journal_replayed = replayed;
        if (replayed) {
            /* The log rewrote blocks under the cache -- the superblock's
             * among them, perhaps -- so everything is read again. */
            bcache_reset();
            if (sb_read() != 0) {
                mounted = 0;
                journal_on = 0;
                return -EIO;
            }
            free_blocks = le32(s + SB_FREE_BLOCKS);
            free_inodes = le32(s + SB_FREE_INODES);
            read_label();
        }
        /*
         * A volume that stopped while THIS driver had it, journal and
         * all, is consistent once the log is replayed: that is the
         * point of the journal, and the check is not run. One that
         * stopped under a driver without the journal (needs_recovery
         * clear, state dirty) is checked as before.
         */
        if (journal_on && (incompat & FEAT_INCOMPAT_RECOVER)) {
            was_unclean = 0;
        }
        if (journal_on) {
            put_le32(sbp(SB_FEATURE_INCOMPAT),
                     le32(s + SB_FEATURE_INCOMPAT) | FEAT_INCOMPAT_RECOVER);
        }
    }

    /* Read-only: whatever the log held has been replayed (Linux does
     * that too), and from here nothing is written -- not even the
     * mark that says the volume is in use. */
    if (V->rdonly) {
        journal_on = 0;
        return 0;
    }
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
    if (fast_symlink(&ei)) {
        memset(ei.block, 0, sizeof(ei.block));  /* a target, not blocks */
    } else {
        inode_truncate_blocks(&ei, 0);
    }
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
    /*
     * A SETGID DIRECTORY gives what is made in it its own group, and a
     * directory made in it the bit as well -- BSD's rule, which Linux
     * follows, so that a shared directory stays shared all the way
     * down. Without it a file made here by one user was in that user's
     * group, and Linux reading the volume saw the difference
     * (kernel/linuxfstest.sh).
     */
    {
        struct einode parent;

        if (iread(dino, &parent) == 0 && (parent.mode & S_ISGID)) {
            gid = parent.gid;
            if (S_ISDIR(mode)) {
                mode |= S_ISGID;
            }
        }
    }
    memset(&ei, 0, sizeof(ei));
    ei.ino = ino;
    ei.mode = mode;
    ei.uid = uid;
    ei.gid = gid;
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
    u32 ino;
    s32 n;
    int err;

    if (!enter()) {
        return -ENODEV;
    }
    ino = sel((u32)f->priv);
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
    u32 ino;
    s32 n;
    int err;

    if (!enter()) {
        return -ENODEV;
    }
    ino = sel((u32)f->priv);
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
    u32 ino = sel((u32)f->priv);
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
    st->st_dev = V->st_dev;
}

static int ext2_file_fstat(struct file *f, struct stat *st)
{
    struct einode ei;
    int err = iread(sel((u32)f->priv), &ei);

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
    err = iread(sel((u32)f->priv), &ei);
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
    u32 ino = sel((u32)f->priv);
    struct einode ei;

    open_unref(ino);
    if (open_count(ino) == 0 && iread(ino, &ei) == 0 && ei.links == 0) {
        orphan_remove(ino);
        inode_release(ino);
    }
    op_flush();
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

    if (!enter()) {
        return -ENODEV;
    }
    err = path_walk(go_cwd(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir) {
        return -EISDIR;         /* "/etc/" is a directory, not a file */
    }
    nlen = name_len_of(nm);

    err = lookup_x(dino, nm, nlen, &ino, &type);
    if (err == -ENOENT) {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        if (!can_write(flags)) {
            return -EACCES;
        }
        if (V->rdonly) {
            return -EROFS;
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
            if (err == -ENOENT && (flags & O_CREAT)) {
                /*
                 * A DANGLING link opened to create: what is created is
                 * the file the link names, as on Linux -- `echo x > link`
                 * makes the target. The target is absolute, or relative
                 * to the link's own directory; a link to a link is
                 * followed the same way, as far as the limit Linux has.
                 */
                static int depth;
                char target[PATH_MAX], next[PATH_MAX];
                u32 dlen = 0, i;

                for (i = 0; path[i]; i++) {
                    if (path[i] == '/') {
                        dlen = i + 1;   /* up to and with the last slash */
                    }
                }

                if (depth >= 8) {
                    return -ELOOP;
                }
                err = read_link(ino, target, sizeof(target));
                if (err != 0) {
                    return err;
                }
                if (target[0] == '/') {
                    strcpy(next, target);
                } else {
                    if (dlen + strlen(target) + 1 > sizeof(next)) {
                        return -ENAMETOOLONG;
                    }
                    memcpy(next, path, dlen);
                    strcpy(next + dlen, target);
                }
                depth++;
                err = ext2_open(next, flags, f);
                depth--;
                return err;
            }
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

    if (V->rdonly && (can_write(flags) || (flags & O_TRUNC))) {
        return -EROFS;
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
    f->priv = (void *)hnd(ino);
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

    if (!enter()) {
        return -ENODEV;
    }
    err = path_walk(go_cwd(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (V->rdonly) {
        return -EROFS;
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
    return op_flush();
}

static int ext2_mkdir(const char *path)
{
    char nm[NAME_MAX + 1];
    u32 dino, ino, nlen, blk, junk;
    u8 type;
    int last_is_dir, err;
    struct einode ei, pe;
    struct bbuf *b;

    if (!enter()) {
        return -ENODEV;
    }
    err = path_walk(go_cwd(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (V->rdonly) {
        return -EROFS;
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
    return op_flush();
}

static int ext2_rmdir(const char *path)
{
    char nm[NAME_MAX + 1];
    u32 dino, ino, nlen;
    u8 type;
    int last_is_dir, err;
    struct einode ei, pe;

    if (!enter()) {
        return -ENODEV;
    }
    err = path_walk(go_cwd(), path, &dino, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (V->rdonly) {
        return -EROFS;
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
    if (at_root(ino) || hnd(ino) == cwd_handle() || is_mountpoint(ino)) {
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
    return op_flush();
}

/* Is `anc` an ancestor of `c`?  Renaming a directory into its own
 * subtree would detach both from the root, so it is refused. */
static int dir_is_within(u32 c, u32 anc)
{
    int guard = 0;

    while (!at_root(c) && ++guard < 256) {
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
    struct ext2_vol *fvol;

    if (!enter()) {
        return -ENODEV;
    }
    err = path_walk(go_cwd(), from, &fdir, fnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || fnm[0] == '\0') {
        return -EINVAL;
    }
    fvol = V;
    err = path_walk(go_cwd(), to, &tdir, tnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (V != fvol) {
        return -EXDEV;          /* a link or a move never crosses volumes */
    }
    if (V->rdonly) {
        return -EROFS;
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
    return op_flush();
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
/* An inode with no data: all a FIFO is on the disk. The directory
 * entry carries its type, as e2fsck checks. */
static int ext2_mknod(const char *path, u32 mode)
{
    char nm[NAME_MAX + 1];
    u32 dir, ino, nl;
    int last_is_dir, err;

    if (!enter()) {
        return -ENODEV;
    }
    if (!S_ISFIFO(mode)) {
        return -EPERM;
    }
    err = path_walk(go_cwd(), path, &dir, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (V->rdonly) {
        return -EROFS;
    }
    if (last_is_dir || nm[0] == '\0') {
        return -EEXIST;
    }
    nl = name_len_of(nm);
    if (dir_lookup(dir, nm, nl, &ino, 0) == 0) {
        return -EEXIST;
    }
    return make_inode(dir, nm, nl, (u16)(S_IFIFO | (mode & 07777)),
                      EXT2_FT_FIFO, &ino);
}

static int ext2_symlink(const char *target, const char *linkpath)
{
    char nm[NAME_MAX + 1];
    u32 dir, ino, nl, tlen;
    int last_is_dir, err;
    struct einode ei;

    if (!enter()) {
        return -ENODEV;
    }
    tlen = (u32)strlen(target);
    if (tlen == 0) {
        return -ENOENT;         /* an empty target names nothing */
    }
    if (tlen > PATH_MAX - 1) {
        return -ENAMETOOLONG;
    }
    err = path_walk(go_cwd(), linkpath, &dir, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (V->rdonly) {
        return -EROFS;
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
    return op_flush();
}

/* stat, without following a symlink in the last component. */
static int ext2_lstat(const char *path, struct stat *st)
{
    struct einode ei;
    u32 ino;
    int err;

    if (!enter()) {
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

    if (!enter()) {
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
    struct ext2_vol *fvol;

    if (!enter()) {
        return -ENODEV;
    }
    err = path_walk(go_cwd(), from, &fdir, fnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir || fnm[0] == '\0') {
        return -EINVAL;
    }
    fvol = V;
    err = path_walk(go_cwd(), to, &tdir, tnm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (V != fvol) {
        return -EXDEV;          /* a link or a move never crosses volumes */
    }
    if (V->rdonly) {
        return -EROFS;
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
    if (is_mountpoint(fino)) {
        return -EBUSY;
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
        if (is_mountpoint(tino)) {
            return -EBUSY;
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
    return op_flush();
}

static int ext2_stat(const char *path, struct stat *st)
{
    struct einode ei;
    u32 ino;
    int err;

    if (!enter()) {
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

    if (!enter()) {
        return -ENODEV;
    }
    ino = (ino == 0) ? go_root() : sel(ino);
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
    return ext2_readdir_in(cwd_handle(), index, out);
}

static int ext2_dir_ino(const char *path, u32 *ino)
{
    struct einode ei;
    u32 n;
    int err;

    if (!enter()) {
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
    *ino = hnd(n);              /* a handle: the volume goes with it */
    return 0;
}

static int ext2_dir_path_op(u32 ino, char *out, u32 size)
{
    if (!enter()) {
        return -ENODEV;
    }
    ino = (ino == 0) ? go_root() : sel(ino);
    return ext2_dir_path(ino, out, size);
}

static int ext2_chdir(const char *path)
{
    char cwd_path[PATH_MAX];
    struct einode ei;
    u32 ino, h;
    int err;

    if (!enter()) {
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
    /* The handle BEFORE the path: naming a directory on a mounted
     * volume walks up out of it, and leaves V on the volume below. */
    h = hnd(ino);
    err = ext2_dir_path(ino, cwd_path, sizeof(cwd_path));
    if (err != 0) {
        return err;
    }
    vfs_cwd_set(h, cwd_path);
    return 0;
}

static const char *ext2_getcwd(void)
{
    return vfs_cwd_path();
}

static int ext2_statfs(const char *path, struct statfs *s)
{
    u32 ino;

    if (!enter()) {
        return -ENODEV;
    }
    if (path) {
        int err = path_resolve(path, &ino);    /* leaves V at its volume */

        if (err != 0) {
            return err;
        }
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
    s->f_flags = V->rdonly ? MS_RDONLY : 0;
    return 0;
}

static int ext2_label(const char *path, struct fslabel *l)
{
    u32 i;

    if (!enter()) {
        return -ENODEV;
    }
    if (path) {
        int err = path_resolve(path, &i);      /* leaves V at its volume */

        if (err != 0) {
            return err;
        }
    }
    for (i = 0; i < sizeof(l->name) - 1 && i < 16; i++) {
        l->name[i] = volume_label[i];
    }
    l->name[i] = '\0';
    return 0;
}

static int sync_one(void)
{
    int a, b;

    if (!mounted || V->rdonly) {
        return 0;
    }
    committing_for_sync = 1;
    a = bcache_flush_all();
    committing_for_sync = 0;
    b = sb_write();
    return (a != 0) ? a : b;
}

static int ext2_sync(void)
{
    int i, err = 0;

    for (i = 0; i < EXT2_MAX_VOLS; i++) {
        if (vols[i]) {
            int e;

            V = vols[i];
            e = sync_one();
            if (e != 0 && err == 0) {
                err = e;
            }
        }
    }
    V = &vol0;
    return err;
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
    int err;

    if (V->rdonly) {
        return -EROFS;
    }
    err = iread(ino, &ei);
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
    return op_flush();
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
    int err;

    if (V->rdonly) {
        return -EROFS;
    }
    err = iread(ino, &ei);
    if (err != 0) {
        return err;
    }
    if (mask & ATTR_MODE) {
        ei.mode = (u16)((ei.mode & S_IFMT) | (mode & 07777));
    }
    if (mask & ATTR_UID) {
        ei.uid = uid;
    }
    if (mask & ATTR_GID) {
        ei.gid = gid;
    }
    /* ctime is "when the inode last changed", which is exactly this. */
    ei.ctime = now_secs();
    err = iwrite(&ei);
    if (err != 0) {
        return err;
    }
    return op_flush();
}

static int ext2_setattr(const char *path, u32 mask, u32 mode, u32 uid, u32 gid)
{
    u32 ino;
    int err;

    if (!enter()) {
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
    if (!enter()) {
        return -ENODEV;
    }
    return set_attr(sel((u32)f->priv), mask, mode, uid, gid);
}

static int ext2_utime(const char *path, u32 mtime, u32 atime)
{
    u32 ino;
    int err;

    if (!enter()) {
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
    if (!enter()) {
        return -ENODEV;
    }
    return set_times(sel((u32)f->priv), mtime, atime);
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

    if (!enter()) {
        return -ENODEV;
    }
    err = iread(sel((u32)f->priv), &ei);
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

    if (fast_symlink(ei)) {
        return 0;               /* its "pointers" are its target's bytes */
    }

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

    if (dir_lookup(EXT2_ROOT_INO, "lost+found", 10, &lf, &type) != 0) {
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
        u32 blk, p = 0, prev = 0xffffffffUL;

        if (bmap(&di, off / block_size, 0, &blk) != 0 || blk == 0) {
            continue;
        }
        while (p + DE_MIN_SIZE <= block_size) {
            struct bbuf *b = bget(blk);
            char name[NAME_MAX + 1];
            struct einode ce;
            u8 *ent;
            u32 rec, child, nl, cur = p;

            if (!b) {
                return;
            }
            ent = b->data + p;
            rec = le16(ent + DE_REC_LEN);
            child = le32(ent + DE_INODE);
            nl = ent[DE_NAME_LEN];

            /*
             * A TORN ENTRY: a length that cannot be, a name longer than
             * its slot, an inode number past the end. Nothing after it in
             * the block can be trusted to be where it seems, so it is
             * salvaged as e2fsck does -- the entry before it takes the
             * rest of the block (or, first in the block, it becomes one
             * empty entry). Names lost with it leave their inodes
             * unattached, and the sweep reconnects those to /lost+found.
             * Before this the walk simply stopped: everything after the
             * damage went uncounted, and nothing repaired the block.
             */
            if (rec < DE_MIN_SIZE || (rec & 3) != 0 || p + rec > block_size ||
                DE_NAME + nl > rec || child > inodes_count) {
                r->bad_entries++;
                if (fsck_repairing) {
                    if (prev != 0xffffffffUL) {
                        put_le16(b->data + prev + DE_REC_LEN,
                                 (u16)(block_size - prev));
                    } else {
                        put_le32(ent + DE_INODE, 0);
                        put_le16(ent + DE_REC_LEN, (u16)(block_size - p));
                    }
                    bdirty(b);
                    r->fixed++;
                }
                break;
            }
            memcpy(name, ent + DE_NAME, nl);
            name[nl] = '\0';
            p += rec;           /* advance BEFORE anything may recurse */

            if (child == 0) {
                prev = cur;
                continue;
            }
            if (nl == 1 && name[0] == '.') {
                if (child != dino) {
                    r->dot_entries++;
                    bad_dots = 1;
                }
                prev = cur;
                continue;       /* not a name for counting purposes   */
            }
            if (nl == 2 && name[0] == '.' && name[1] == '.') {
                if (child != parent) {
                    r->dot_entries++;
                    bad_dots = 1;
                }
                prev = cur;
                continue;
            }
            if (!inode_in_use(child)) {
                int in_bitmap;

                /*
                 * The bitmap says free. If the inode TABLE says the file
                 * is live, the bitmap is what is wrong -- the sweep sets
                 * the bit -- and this name counts. Counting it as an
                 * orphan instead left the file with no names, so it was
                 * reconnected to /lost+found as well, and the next check
                 * found one link too many.
                 */
                if (!fsck_inode_live(child, &ce, &in_bitmap)) {
                    r->orphan_names++;
                    if (fsck_repairing) {
                        /* A name for nothing: removed, as e2fsck does. */
                        b = bget(blk);
                        if (b) {
                            if (prev != 0xffffffffUL) {
                                put_le16(b->data + prev + DE_REC_LEN,
                                         (u16)(le16(b->data + prev + DE_REC_LEN) + rec));
                            } else {
                                put_le32(b->data + cur + DE_INODE, 0);
                            }
                            bdirty(b);
                            r->fixed++;
                        }
                    } else {
                        prev = cur;
                    }
                    continue;
                }
            }
            prev = cur;
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

static int check_volume(int flags, struct fsck_report *r);
static void umount_one(void);
static void vol_free(struct ext2_vol *v);

static int ext2_check(int flags, struct fsck_report *r)
{
    if (!enter()) {
        return -ENODEV;
    }
    return check_volume(flags, r);
}

/*
 * CHECK A VOLUME THAT IS NOT MOUNTED -- fsck /dev/hda2, as on Linux,
 * where fsck is run on a device before it is mounted. It is mounted for
 * the length of the check, attached to nothing: read-only unless
 * repairing, so a look changes nothing on the disk; with a repair, as
 * a writable mount would, its journal replayed and the volume left
 * marked clean. A device whose sectors are mounted is refused (EBUSY):
 * checking a volume underneath its own mount is how checks corrupt.
 */
static int ext2_check_dev(struct blockdev *b, int flags, struct fsck_report *r)
{
    struct ext2_vol *v;
    struct blockdev *disk;
    u32 start, lba, pages, pa;
    int i, slot = -1, err;

    if (!enter()) {
        return -ENODEV;
    }
    if (!b || !b->read || b->sector_size != SECTOR_SIZE) {
        return -ENXIO;
    }
    if ((flags & FSCK_REPAIR) && !b->write) {
        return -EROFS;
    }
    for (i = 1; i < EXT2_MAX_VOLS; i++) {
        if (!vols[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -EMFILE;
    }
    pages = (sizeof(struct ext2_vol) + PAGE_SIZE - 1) / PAGE_SIZE;
    pa = pmm_alloc_pages(pages);
    if (!pa) {
        return -ENOMEM;
    }
    v = (struct ext2_vol *)pa;
    memset(v, 0, sizeof(*v));
    v->idx = slot;
    v->pages = pages;
    v->parent = 0;
    v->rdonly = !(flags & FSCK_REPAIR);
    v->st_dev = dev_block_rdev(b);

    V = v;
    dev = b;
    lba = find_partition();
    disk = dev_block_base(b, &start);
    for (i = 0; i < EXT2_MAX_VOLS; i++) {
        u32 s2;

        if (vols[i] && vols[i]->v_mounted &&
            dev_block_base(vols[i]->v_dev, &s2) == disk &&
            s2 + vols[i]->v_part_lba == start + lba) {
            pmm_free_pages(pa, pages);
            V = &vol0;
            return -EBUSY;
        }
    }
    vols[slot] = v;
    err = ext2_mount_dev();
    if (err == 0) {
        err = check_volume(flags, r);
        V = v;
        umount_one();
    } else {
        V = v;
        mounted = 0;
    }
    vol_free(v);
    return err;
}

static int check_volume(int flags, struct fsck_report *r)
{
    u32 seen_bytes, g, ino, b, used = 0, counted_free = 0;
    u32 inodes_per_block = block_size / inode_size;
    int repair = (flags & FSCK_REPAIR) != 0;
    int i, orphan_count = 0;
    /* Inodes no name reaches, dealt with after the block sweep. A disk
     * with more than this many is one for e2fsck, not for us. */
    static u32 orphans[64];
    struct einode ei;

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
    fsck_tree(EXT2_ROOT_INO, EXT2_ROOT_INO, 0, r);
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
    V = &vol0;
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

/* Write everything out, say the volume is clean, and let go of it. */
static void umount_one(void)
{
    if (mounted && !V->rdonly) {
        bcache_flush_all();
        if (journal_on) {
            u8 *s = sbuf + EXT2_SUPER_OFF;

            put_le32(s + SB_FEATURE_INCOMPAT,
                     le32(s + SB_FEATURE_INCOMPAT) & ~(u32)FEAT_INCOMPAT_RECOVER);
            journal_on = 0;
        }
        set_clean(1);           /* everything out, then say so         */
    }
    if (txn_freed) {
        txn_freed_clear();
        pmm_free((u32)txn_freed);
        txn_freed = 0;
    }
    if (jmap) {
        pmm_free_pages((u32)jmap, jmap_pages);
        jmap = 0;
    }
    journal_on = 0;
    mounted = 0;
    dev = 0;
}

/* Let go of a mounted volume's memory and its slot. */
static void vol_free(struct ext2_vol *v)
{
    vols[v->idx] = 0;
    pmm_free_pages((u32)v, v->pages);
    V = &vol0;
}

/* The whole tree, as the machine stops: the mounted volumes first, the
 * most recently mounted first, so nothing is let go while something is
 * still mounted on it. */
static int ext2_umount(void)
{
    int i;

    for (i = EXT2_MAX_VOLS - 1; i > 0; i--) {
        if (vols[i]) {
            V = vols[i];
            umount_one();
            vol_free(vols[i]);
        }
    }
    V = &vol0;
    umount_one();
    return 0;
}

/*
 * MOUNT: the volume on `b` at directory `dir`.
 *
 * The directory must exist, must not be the root of a volume (stacking
 * a mount on another's root is something Linux allows and nothing here
 * needs), and so cannot already have something on it -- a walk to it
 * would have crossed. The device must not hold a volume that is already
 * mounted, by any of its names: the root is mounted from "hda" and
 * lives in hda1, so the check is on the sectors, not the name.
 */
static int ext2_mount_on(const char *dir, struct blockdev *b, u32 flags)
{
    struct ext2_vol *parent, *v;
    struct blockdev *disk;
    u32 ino, h, start, lba, pages, pa;
    struct einode ei;
    int i, slot = -1, err;

    if (!enter()) {
        return -ENODEV;
    }
    if (!b || !b->read || b->sector_size != SECTOR_SIZE) {
        return -ENXIO;
    }
    if ((flags & ~(u32)MS_RDONLY) != 0) {
        return -EINVAL;
    }
    if (!b->write) {
        flags |= MS_RDONLY;
    }
    err = path_resolve(dir, &ino);
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
    if (ino == EXT2_ROOT_INO) {
        return -EBUSY;          /* a volume's root: something is here  */
    }
    parent = V;
    h = hnd(ino);

    for (i = 1; i < EXT2_MAX_VOLS; i++) {
        if (!vols[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -EMFILE;
    }

    pages = (sizeof(struct ext2_vol) + PAGE_SIZE - 1) / PAGE_SIZE;
    pa = pmm_alloc_pages(pages);
    if (!pa) {
        return -ENOMEM;
    }
    v = (struct ext2_vol *)pa;
    memset(v, 0, sizeof(*v));
    v->idx = slot;
    v->pages = pages;
    v->parent = parent;
    v->mp = h;
    v->rdonly = (flags & MS_RDONLY) != 0;
    v->st_dev = dev_block_rdev(b);

    /* The sectors it would use, against every volume mounted already. */
    V = v;
    dev = b;
    lba = find_partition();
    disk = dev_block_base(b, &start);
    for (i = 0; i < EXT2_MAX_VOLS; i++) {
        u32 s2;

        if (vols[i] && vols[i]->v_mounted &&
            dev_block_base(vols[i]->v_dev, &s2) == disk &&
            s2 + vols[i]->v_part_lba == start + lba) {
            pmm_free_pages(pa, pages);
            V = &vol0;
            return -EBUSY;
        }
    }

    vols[slot] = v;
    err = ext2_mount_dev();
    if (err == 0 && !v->rdonly) {
        orphan_process();
        bcache_flush_all();
        sb_write();
    }
    if (err != 0) {
        V = v;
        mounted = 0;
        vol_free(v);
        return err;
    }
    V = &vol0;
    return 0;
}

/*
 * Is volume `v` in use: a file open on it, a volume mounted on it, or a
 * task whose working directory or root is on it (asked of the VFS,
 * which knows the tasks). A program running from it holds its file
 * open, so that is the first case.
 */
static int vol_in_use(struct ext2_vol *v)
{
    u32 lo = (u32)v->idx << VOL_SHIFT;
    int i;

    for (i = 0; i < EXT2_MAX_OPEN; i++) {
        if (v->v_opened[i].refs > 0) {
            return 1;
        }
    }
    for (i = 1; i < EXT2_MAX_VOLS; i++) {
        if (vols[i] && vols[i]->parent == v) {
            return 1;
        }
    }
    return vfs_handles_in(lo, lo | INO_MASK);
}

/*
 * REMOUNT a volume read-only, or writable again.
 *
 * Read-only is what unmounting does to it short of letting go: every
 * block out, the journal committed and closed (needs_recovery cleared),
 * the superblock marked clean -- so a machine that stops while it is
 * read-only leaves a volume nothing has to check or replay. Refused
 * while any file on it is open for writing, as on Linux.
 *
 * Writable again is the end of a mount: the journal set up afresh (it
 * holds nothing, having been closed cleanly), needs_recovery set, the
 * volume marked in use, and orphans dealt with.
 */
static int ext2_remount(const char *dir, u32 flags)
{
    u32 ino, lo;
    int err, ro = (flags & MS_RDONLY) != 0;

    if (!enter()) {
        return -ENODEV;
    }
    if ((flags & ~(u32)MS_RDONLY) != 0) {
        return -EINVAL;
    }
    err = path_resolve(dir, &ino);
    if (err != 0) {
        V = &vol0;
        return err;
    }
    if (ino != EXT2_ROOT_INO) {
        V = &vol0;
        return -EINVAL;         /* not the root of a volume            */
    }
    if (ro == V->rdonly) {
        V = &vol0;
        return 0;
    }
    if (ro) {
        lo = (u32)V->idx << VOL_SHIFT;
        if (vfs_writers_in(lo, lo | INO_MASK)) {
            V = &vol0;
            return -EBUSY;
        }
        bcache_flush_all();
        if (journal_on) {
            u8 *s = sbuf + EXT2_SUPER_OFF;

            put_le32(s + SB_FEATURE_INCOMPAT,
                     le32(s + SB_FEATURE_INCOMPAT) & ~(u32)FEAT_INCOMPAT_RECOVER);
            journal_on = 0;
        }
        set_clean(1);
        V->rdonly = 1;
    } else {
        u32 replayed = 0;

        V->rdonly = 0;
        if (journal_setup(&replayed) != 0) {
            journal_on = 0;     /* as mount does with one it cannot use */
        }
        if (journal_on) {
            put_le32(sbp(SB_FEATURE_INCOMPAT),
                     le32(sbuf + EXT2_SUPER_OFF + SB_FEATURE_INCOMPAT) |
                     FEAT_INCOMPAT_RECOVER);
        }
        set_clean(0);
        orphan_process();
        bcache_flush_all();
        sb_write();
    }
    V = &vol0;
    return 0;
}

static int ext2_umount_on(const char *dir, u32 flags)
{
    struct ext2_vol *v;
    u32 ino;
    int err;

    if (!enter()) {
        return -ENODEV;
    }
    if ((flags & ~(u32)MNT_FORCE) != 0) {
        return -EINVAL;         /* MNT_DETACH, MNT_EXPIRE: not here     */
    }
    err = path_resolve(dir, &ino);
    if (err != 0) {
        return err;
    }
    v = V;
    if (ino != EXT2_ROOT_INO || !v->parent) {
        V = &vol0;
        return -EINVAL;         /* not a mount point (or the root)     */
    }
    if (vol_in_use(v)) {
        V = &vol0;
        return -EBUSY;
    }
    umount_one();
    vol_free(v);
    return 0;
}

static int ext2_mount_list(int i, struct mount_entry *m)
{
    struct ext2_vol *v;
    int n = 0, k;

    if (!enter()) {
        return -ENOENT;
    }
    for (k = 0; k < EXT2_MAX_VOLS; k++) {
        if (vols[k] && vols[k]->v_mounted && n++ == i) {
            break;
        }
    }
    if (k == EXT2_MAX_VOLS) {
        return -ENOENT;
    }
    v = vols[k];
    memset(m, 0, sizeof(*m));
    strncpy(m->source, v->v_dev->name, sizeof(m->source) - 1);
    m->flags = v->rdonly ? MS_RDONLY : 0;
    /* ext3's on-disk format is ext2's plus a journal, and Linux reports
     * a volume by the format -- the has_journal feature, not whether the
     * journal is running, which it is not while read-only: df -T and
     * findmnt read it from here. */
    strcpy(m->type, (le32(v->v_sbuf + EXT2_SUPER_OFF + SB_FEATURE_COMPAT) &
                     FEAT_COMPAT_HAS_JOURNAL) ? "ext3" : "ext2");
    if (!v->parent) {
        strcpy(m->dir, "/");
        /* The root is mounted from the whole disk and lives in its
         * first Linux partition: name the partition, as Linux would. */
        if (v->v_part_lba) {
            u32 nl = (u32)strlen(m->source);

            if (nl + 1 < sizeof(m->source)) {
                m->source[nl] = '1';
                m->source[nl + 1] = '\0';
            }
        }
    } else {
        /* Where it is. A task in a chroot sees it from its own root,
         * which is what Linux's /proc/self/mounts does too. */
        V = v;
        if (ext2_dir_path(EXT2_ROOT_INO, m->dir, sizeof(m->dir)) != 0) {
            strcpy(m->dir, "?");
        }
    }
    V = &vol0;
    return 0;
}

static struct fs_type ext2_fs = {
    .name = "ext2",
    .mount = ext2_mount,
    .umount = ext2_umount,
    .open = ext2_open,
    .unlink = ext2_unlink,
    .rename = ext2_rename,
    .stat = ext2_stat,
    .readdir = ext2_readdir,
    .statfs = ext2_statfs,
    .remount = ext2_remount,
    .check_dev = ext2_check_dev,
    .sync = ext2_sync,
    .mkdir = ext2_mkdir,
    .rmdir = ext2_rmdir,
    .chdir = ext2_chdir,
    .getcwd = ext2_getcwd,
    .readdir_in = ext2_readdir_in,
    .dir_ino = ext2_dir_ino,
    .dir_path = ext2_dir_path_op,
    .utime = ext2_utime,
    .futime = ext2_futime,
    .setattr = ext2_setattr,
    .fsetattr = ext2_fsetattr,
    .link = ext2_link,
    .symlink = ext2_symlink,
    .readlink = ext2_readlink,
    .lstat = ext2_lstat,
    .check = ext2_check,
    .label = ext2_label,
    .bmap = ext2_bmap,
    .mknod = ext2_mknod,
    .boundary = ext2_boundary,
    .mount_on = ext2_mount_on,
    .umount_on = ext2_umount_on,
    .mount_list = ext2_mount_list,
};

int ext2_init(void)
{
    return vfs_register(&ext2_fs);
}
