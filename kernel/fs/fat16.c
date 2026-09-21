/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fat16.c - FAT16, read and write.
 *
 * Registered with the VFS as the filesystem type "fat16" and mounted on
 * a struct blockdev.  It never names a disk controller, and nothing
 * above it names FAT: the system call layer holds a struct file with a
 * struct file_ops, and a different filesystem is a different file with
 * the same shape.
 *
 * References: Microsoft FAT32 File System Specification (which documents
 * FAT12 and FAT16 alongside it), and the MS-DOS 3.3 disk format it came
 * from.
 *
 * The disk is a genuine MS-DOS disk and stays that way.  Every structure
 * written here is one that mtools, fsck.fat and MS-DOS itself will
 * accept, which is worth more than any convenience a private format
 * would buy: the host can drop a file on the disk and this kernel reads
 * it, and anything the kernel writes can be pulled off the image
 * afterwards without the kernel running.
 *
 * Deliberate limits, none of which change the calls in fs.h:
 *   - root directory only, no subdirectories
 *   - 8.3 names, no long names (long-name entries are skipped on scan,
 *     so a file the host created with one is still visible by its short
 *     name and is not corrupted)
 *   - no FAT12 or FAT32
 *
 * Two things are easy to get wrong here and both are handled explicitly:
 *
 *   BYTE ORDER.  Every multi-byte field on a FAT disk is little-endian
 *   and this CPU is not, so nothing is read by casting a pointer.  It
 *   all goes through le16()/le32() and their write counterparts, which
 *   work a byte at a time and so are indifferent to alignment as well.
 *
 *   THE SECOND FAT.  A FAT16 volume normally carries two copies of the
 *   table.  Updating only the first leaves a disk that works until
 *   something checks, and then fails a check it should pass.  Every FAT
 *   write goes to all copies.
 */
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
    p[1] = (u8)(v >> 8);
}

static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
    p[2] = (u8)((v >> 16) & 0xff);
    p[3] = (u8)((v >> 24) & 0xff);
}

/* ---------------------------------------------------------------- */
/* Volume state                                                      */
/* ---------------------------------------------------------------- */

#define SECTOR_SIZE    512
#define FAT_MAX_OPEN   OPEN_MAX
#define FAT_EOC        0xfff8u     /* >= this ends a chain */
#define FAT_BAD        0xfff7u
#define DIRENT_SIZE    32
#define ATTR_RDONLY    0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIR       0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0f

/*
 * Timestamps come from whatever clock registered itself.  If the clock does not answer, or
 * answers with something that is not a date, files get this fixed stamp
 * instead: wrong but constant, which reads as obviously synthetic rather
 * than quietly plausible, and is a visible sign that the clock needs
 * setting.
 */
#define FS_FALLBACK_DATE  ((u16)(((2026 - 1980) << 9) | (1 << 5) | 1))

static void fs_now(u16 *date, u16 *time)
{
    struct rtcdev *r = dev_rtc();
    struct tm now;
    time_t secs;

    if (r && r->get(r, &secs) == 0) {
        gmtime_r(secs, &now);
        *date = tm_to_fat_date(&now);
        *time = tm_to_fat_time(&now);
        if (*date != 0) {
            return;
        }
    }
    *date = FS_FALLBACK_DATE;
    *time = 0;
}

static struct blockdev *dev;
static int  mounted;
static u32  part_lba;
static u32  fat_start;          /* LBA of the first FAT               */
static u32  root_start;         /* LBA of the root directory          */
static u32  data_start;         /* LBA of cluster 2                   */
static u32  sectors_per_fat;
static u32  root_entries;
static u32  root_sectors;
static u32  total_clusters;     /* highest valid cluster is this + 1  */
static u32  cluster_bytes;
static u8   sectors_per_cluster;
static u8   num_fats;
static u16  bytes_per_sector;
static u32  alloc_hint = 2;
static char volume_label[12];

/* ---------------------------------------------------------------- */
/* Sector buffers                                                    */
/*                                                                    */
/* Two: one for the FAT, one for everything else.  Each remembers what */
/* it holds, so a scan that stays inside a sector costs one read.      */
/* ---------------------------------------------------------------- */

struct sbuf {
    u8  data[SECTOR_SIZE];
    u32 lba;
    u8  valid;
    u8  dirty;
};

static struct sbuf sb;          /* directory and file data            */
static struct sbuf fb;          /* FAT, addressed relative to fat_start */

static int sb_flush(void)
{
    if (sb.valid && sb.dirty) {
        if (dev->write(dev, sb.lba, 1, sb.data) != 0) {
            return -EIO;
        }
        sb.dirty = 0;
    }
    return 0;
}

static int sb_get(u32 lba)
{
    int err;

    if (sb.valid && sb.lba == lba) {
        return 0;
    }
    err = sb_flush();
    if (err != 0) {
        return err;
    }
    if (dev->read(dev, lba, 1, sb.data) != 0) {
        sb.valid = 0;
        return -EIO;
    }
    sb.lba = lba;
    sb.valid = 1;
    sb.dirty = 0;
    return 0;
}

/* The FAT buffer is indexed by sector within a FAT, because a flush has
 * to write the same sector into every copy of the table. */
static int fb_flush(void)
{
    u32 i;

    if (!(fb.valid && fb.dirty)) {
        return 0;
    }
    for (i = 0; i < num_fats; i++) {
        if (dev->write(dev, fat_start + i * sectors_per_fat + fb.lba, 1,
                      fb.data) != 0) {
            return -EIO;
        }
    }
    fb.dirty = 0;
    return 0;
}

static int fb_get(u32 rel)
{
    int err;

    if (fb.valid && fb.lba == rel) {
        return 0;
    }
    err = fb_flush();
    if (err != 0) {
        return err;
    }
    if (dev->read(dev, fat_start + rel, 1, fb.data) != 0) {
        fb.valid = 0;
        return -EIO;
    }
    fb.lba = rel;
    fb.valid = 1;
    fb.dirty = 0;
    return 0;
}

static int fat_flush_all(void)
{
    int a = sb_flush();
    int b = fb_flush();

    return (a != 0) ? a : b;
}

/* ---------------------------------------------------------------- */
/* The FAT itself                                                    */
/* ---------------------------------------------------------------- */

static int cluster_valid(u32 cl)
{
    return cl >= 2 && cl <= total_clusters + 1;
}

static int fat_get(u32 cl, u16 *out)
{
    u32 off = cl * 2;
    int err = fb_get(off / bytes_per_sector);

    if (err != 0) {
        return err;
    }
    *out = le16(&fb.data[off % bytes_per_sector]);
    return 0;
}

static int fat_set(u32 cl, u16 val)
{
    u32 off = cl * 2;
    int err = fb_get(off / bytes_per_sector);

    if (err != 0) {
        return err;
    }
    put_le16(&fb.data[off % bytes_per_sector], val);
    fb.dirty = 1;
    return 0;
}

/* Find a free cluster, claim it as the end of a chain, and return it in
 * *out.  The hint makes a sequential write walk the table once rather
 * than restarting from cluster 2 on every extension. */
static int fat_alloc(u32 *out)
{
    u32 tried;
    u32 cl = alloc_hint;

    for (tried = 0; tried < total_clusters; tried++) {
        u16 v;
        int err;

        if (cl > total_clusters + 1) {
            cl = 2;
        }
        err = fat_get(cl, &v);
        if (err != 0) {
            return err;
        }
        if (v == 0) {
            err = fat_set(cl, 0xffff);
            if (err != 0) {
                return err;
            }
            alloc_hint = cl + 1;
            *out = cl;
            return 0;
        }
        cl++;
    }
    return -ENOSPC;
}

static int fat_free_chain(u32 first)
{
    u32 cl = first;

    while (cluster_valid(cl)) {
        u16 next;
        int err = fat_get(cl, &next);

        if (err != 0) {
            return err;
        }
        err = fat_set(cl, 0);
        if (err != 0) {
            return err;
        }
        if (cl < alloc_hint) {
            alloc_hint = cl;
        }
        cl = next;
    }
    return 0;
}

static u32 cluster_lba(u32 cl)
{
    return data_start + (cl - 2) * sectors_per_cluster;
}

/* ---------------------------------------------------------------- */
/* Names                                                             */
/* ---------------------------------------------------------------- */

static char upcase(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int name_char_ok(char c)
{
    if ((unsigned char)c < 0x20) {
        return 0;
    }
    switch (c) {
    case '"': case '*': case '+': case ',': case '/': case ':':
    case ';': case '<': case '=': case '>': case '?': case '[':
    case '\\': case ']': case '|': case ' ': case '.':
        return 0;
    default:
        return 1;
    }
}

/*
 * "kernel.rom" -> "KERNEL  ROM", the eleven bytes a directory entry
 * actually holds.  Returns -EINVAL for anything that is not a legal
 * 8.3 name, rather than silently truncating it into a different file.
 */
static int name_to_83(const char *in, char out[11])
{
    int i = 0, n = 0;

    for (i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    if (in == 0 || *in == '\0') {
        return -EINVAL;
    }

    /* base name */
    for (n = 0; *in && *in != '.'; in++) {
        if (!name_char_ok(*in) || n >= 8) {
            return -EINVAL;
        }
        out[n++] = upcase(*in);
    }
    if (n == 0) {
        return -EINVAL;
    }

    if (*in == '.') {
        in++;
        for (n = 0; *in; in++) {
            if (!name_char_ok(*in) || n >= 3) {
                return -EINVAL;
            }
            out[8 + n] = upcase(*in);
            n++;
        }
        if (n == 0) {
            return -EINVAL;
        }
    }

    /* 0xE5 as the first byte means "deleted", so it is stored as 0x05. */
    if ((u8)out[0] == 0xe5) {
        out[0] = 0x05;
    }
    return 0;
}

static void name_from_83(const u8 raw[11], char out[NAME_MAX + 1])
{
    int i, n = 0;

    for (i = 0; i < 8 && raw[i] != ' '; i++) {
        out[n++] = (char)raw[i];
    }
    if (n > 0 && (u8)out[0] == 0x05) {
        out[0] = (char)0xe5;
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && raw[i] != ' '; i++) {
            out[n++] = (char)raw[i];
        }
    }
    out[n] = '\0';
}

/* ---------------------------------------------------------------- */
/* The root directory                                                */
/* ---------------------------------------------------------------- */

static u32 dir_entry_lba(u32 index)
{
    return root_start + index / (bytes_per_sector / DIRENT_SIZE);
}

static u32 dir_entry_off(u32 index)
{
    return (index % (bytes_per_sector / DIRENT_SIZE)) * DIRENT_SIZE;
}

static int dir_read(u32 index, u8 *ent)
{
    int err;

    if (index >= root_entries) {
        return -ENOENT;
    }
    err = sb_get(dir_entry_lba(index));
    if (err != 0) {
        return err;
    }
    memcpy(ent, &sb.data[dir_entry_off(index)], DIRENT_SIZE);
    return 0;
}

static int dir_write(u32 index, const u8 *ent)
{
    int err;

    if (index >= root_entries) {
        return -ENOENT;
    }
    err = sb_get(dir_entry_lba(index));
    if (err != 0) {
        return err;
    }
    memcpy(&sb.data[dir_entry_off(index)], ent, DIRENT_SIZE);
    sb.dirty = 1;
    return 0;
}

/* Is this entry a real file or directory, as opposed to free, deleted, a
 * long-name fragment or the volume label? */
static int dir_entry_is_file(const u8 *ent)
{
    if (ent[0] == 0x00 || ent[0] == 0xe5) {
        return 0;
    }
    if ((ent[11] & ATTR_LFN) == ATTR_LFN) {
        return 0;
    }
    if (ent[11] & ATTR_VOLUME) {
        return 0;
    }
    return 1;
}

/* Find name83 in the root directory.  Returns the entry index, or
 * -ENOENT. */
static int dir_lookup(const char name83[11], u8 *ent_out)
{
    u32 i;
    u8 ent[DIRENT_SIZE];

    for (i = 0; i < root_entries; i++) {
        int err = dir_read(i, ent);

        if (err != 0) {
            return err;
        }
        if (ent[0] == 0x00) {
            break;              /* nothing is allocated beyond here */
        }
        if (!dir_entry_is_file(ent)) {
            continue;
        }
        if (memcmp(ent, name83, 11) == 0) {
            if (ent_out) {
                memcpy(ent_out, ent, DIRENT_SIZE);
            }
            return (int)i;
        }
    }
    return -ENOENT;
}

/* Claim a free directory slot and write a fresh entry into it. */
static int dir_create(const char name83[11], u8 attr)
{
    u32 i;
    u8 ent[DIRENT_SIZE];
    u16 date, time;

    for (i = 0; i < root_entries; i++) {
        int err = dir_read(i, ent);

        if (err != 0) {
            return err;
        }
        if (ent[0] != 0x00 && ent[0] != 0xe5) {
            continue;
        }

        memset(ent, 0, DIRENT_SIZE);
        memcpy(ent, name83, 11);
        ent[11] = attr;
        fs_now(&date, &time);
        put_le16(&ent[14], time);           /* created */
        put_le16(&ent[16], date);
        put_le16(&ent[18], date);           /* accessed */
        put_le16(&ent[22], time);           /* modified */
        put_le16(&ent[24], date);
        put_le16(&ent[26], 0);              /* first cluster */
        put_le32(&ent[28], 0);              /* size */

        err = dir_write(i, ent);
        if (err != 0) {
            return err;
        }
        return (int)i;
    }
    return -ENOSPC;
}

/* ---------------------------------------------------------------- */
/* Mounting                                                          */
/* ---------------------------------------------------------------- */

/* Read the MBR and return the first FAT partition's start LBA, or 0 if
 * the disk has no partition table and the filesystem starts at sector
 * zero. */
static u32 find_partition(void)
{
    u8 mbr[SECTOR_SIZE];
    int i;

    if (dev->read(dev, 0, 1, mbr) != 0) {
        return 0;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
        return 0;               /* no partition table: superfloppy */
    }
    for (i = 0; i < 4; i++) {
        const u8 *p = &mbr[446 + i * 16];
        u8 type = p[4];
        u32 start = le32(&p[8]);
        u32 size = le32(&p[12]);

        if (size == 0) {
            continue;
        }
        if (type == 0x01 || type == 0x04 || type == 0x06 ||
            type == 0x0e || type == 0x0b || type == 0x0c) {
            return start;
        }
    }
    return 0;
}

static void read_label(void)
{
    u32 i;
    u8 ent[DIRENT_SIZE];

    memset(volume_label, 0, sizeof(volume_label));

    /* The authoritative label is the root directory entry with the
     * volume attribute; the copy in the boot sector can be stale. */
    for (i = 0; i < root_entries; i++) {
        if (dir_read(i, ent) != 0) {
            break;
        }
        if (ent[0] == 0x00) {
            break;
        }
        if (ent[0] == 0xe5) {
            continue;
        }
        if ((ent[11] & ATTR_LFN) == ATTR_LFN) {
            continue;
        }
        if (ent[11] & ATTR_VOLUME) {
            int n;

            memcpy(volume_label, ent, 11);
            volume_label[11] = '\0';
            for (n = 10; n >= 0 && volume_label[n] == ' '; n--) {
                volume_label[n] = '\0';
            }
            return;
        }
    }
}

static int fat_mount_dev(void)
{
    u8 bpb[SECTOR_SIZE];
    u32 total_sectors;
    u32 reserved;
    u32 data_sectors;

    mounted = 0;
    sb.valid = sb.dirty = 0;
    fb.valid = fb.dirty = 0;
    alloc_hint = 2;

    part_lba = find_partition();

    if (dev->read(dev, part_lba, 1, bpb) != 0) {
        return -EIO;
    }

    bytes_per_sector    = le16(&bpb[11]);
    sectors_per_cluster = bpb[13];
    reserved            = le16(&bpb[14]);
    num_fats            = bpb[16];
    root_entries        = le16(&bpb[17]);
    total_sectors       = le16(&bpb[19]);
    sectors_per_fat     = le16(&bpb[22]);
    if (total_sectors == 0) {
        total_sectors = le32(&bpb[32]);
    }

    if (bytes_per_sector != SECTOR_SIZE ||
        sectors_per_cluster == 0 ||
        num_fats == 0 || num_fats > 2 ||
        root_entries == 0 || sectors_per_fat == 0 ||
        reserved == 0 || total_sectors == 0) {
        return -EINVAL;
    }

    fat_start    = part_lba + reserved;
    root_start   = fat_start + (u32)num_fats * sectors_per_fat;
    root_sectors = (root_entries * DIRENT_SIZE + bytes_per_sector - 1) /
                   bytes_per_sector;
    data_start   = root_start + root_sectors;

    data_sectors = total_sectors - (data_start - part_lba);
    total_clusters = data_sectors / sectors_per_cluster;
    cluster_bytes  = (u32)sectors_per_cluster * bytes_per_sector;

    /* FAT16 is defined by the cluster count, not by anything written on
     * the disk.  Refusing FAT12 and FAT32 here is better than misreading
     * one of them as FAT16. */
    if (total_clusters < 4085 || total_clusters >= 65525) {
        return -EINVAL;
    }

    mounted = 1;
    read_label();
    return 0;
}


static const char *fat_label(void)
{
    return volume_label;
}

static u32 fat_cluster_bytes(void)
{
    return mounted ? cluster_bytes : 0;
}

static u32 fat_total_bytes(void)
{
    return mounted ? total_clusters * cluster_bytes : 0;
}

static u32 fat_free_bytes(void)
{
    u32 cl, free_clusters = 0;

    if (!mounted) {
        return 0;
    }
    for (cl = 2; cl <= total_clusters + 1; cl++) {
        u16 v;

        if (fat_get(cl, &v) != 0) {
            return 0;
        }
        if (v == 0) {
            free_clusters++;
        }
    }
    return free_clusters * cluster_bytes;
}

/* ---------------------------------------------------------------- */
/* Open files                                                        */
/* ---------------------------------------------------------------- */

/*
 * POSIX puts the access mode in the low two bits of the open flags
 * rather than giving read and write a bit each, so O_RDONLY is zero and
 * testing for it with & does not work.  These say what was meant.
 */
static int can_read(int flags)
{
    return (flags & O_ACCMODE) != O_WRONLY;
}

static int can_write(int flags)
{
    return (flags & O_ACCMODE) != O_RDONLY;
}

struct fat_file {
    u8  used;
    u8  flags;
    u8  dirty;                  /* directory entry needs rewriting    */
    u32 dir_index;
    u32 first;                  /* first cluster, 0 if empty          */
    u32 size;
    u32 pos;
    u32 cur;                    /* cluster holding cur_index          */
    u32 cur_index;              /* its position in the chain          */
};

static struct fat_file files[FAT_MAX_OPEN];

static struct fat_file *handle(int fd)
{
    if (fd < 0 || fd >= FAT_MAX_OPEN || !files[fd].used) {
        return 0;
    }
    return &files[fd];
}

/* Write the open file's size and first cluster back to its directory
 * entry. */
static int file_sync(struct fat_file *f)
{
    u8 ent[DIRENT_SIZE];
    u16 date, time;
    int err;

    if (!f->dirty) {
        return 0;
    }
    err = dir_read(f->dir_index, ent);
    if (err != 0) {
        return err;
    }
    fs_now(&date, &time);
    put_le16(&ent[26], (u16)f->first);
    put_le32(&ent[28], f->size);
    put_le16(&ent[22], time);
    put_le16(&ent[24], date);
    ent[11] |= ATTR_ARCHIVE;
    err = dir_write(f->dir_index, ent);
    if (err != 0) {
        return err;
    }
    f->dirty = 0;
    return fat_flush_all();
}

/*
 * Find the cluster holding chain position `want`.
 *
 * Returns 0 with *out set, 1 if the chain ends before that position
 * and alloc was not asked for, or a negative error.
 */
static int chain_seek(struct fat_file *f, u32 want, int alloc, u32 *out)
{
    if (f->first == 0) {
        u32 c;
        int err;

        if (!alloc) {
            return 1;
        }
        err = fat_alloc(&c);
        if (err != 0) {
            return err;
        }
        f->first = c;
        f->cur = c;
        f->cur_index = 0;
        f->dirty = 1;
    }

    if (!cluster_valid(f->cur) || want < f->cur_index) {
        f->cur = f->first;
        f->cur_index = 0;
    }

    while (f->cur_index < want) {
        u16 next;
        int err = fat_get(f->cur, &next);

        if (err != 0) {
            return err;
        }
        if (next >= FAT_EOC) {
            u32 c;

            if (!alloc) {
                return 1;
            }
            err = fat_alloc(&c);
            if (err != 0) {
                return err;
            }
            err = fat_set(f->cur, (u16)c);
            if (err != 0) {
                return err;
            }
            next = (u16)c;
        } else if (!cluster_valid(next)) {
            return -EIO;      /* a cross-linked or damaged chain */
        }
        f->cur = next;
        f->cur_index++;
    }

    *out = f->cur;
    return 0;
}

/* Is this directory entry already open?  Used to keep a file from being
 * deleted or renamed while something holds it. */
static int entry_is_open(u32 dir_index)
{
    int i;

    for (i = 0; i < FAT_MAX_OPEN; i++) {
        if (files[i].used && files[i].dir_index == dir_index) {
            return 1;
        }
    }
    return 0;
}

static int fat_handle_open(const char *name, int flags)
{
    char n83[11];
    u8 ent[DIRENT_SIZE];
    struct fat_file *f;
    int fd, idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = name_to_83(name, n83);
    if (err != 0) {
        return err;
    }
    for (fd = 0; fd < FAT_MAX_OPEN; fd++) {
        if (!files[fd].used) {
            break;
        }
    }
    if (fd == FAT_MAX_OPEN) {
        return -EMFILE;
    }
    f = &files[fd];

    idx = dir_lookup(n83, ent);
    if (idx == -ENOENT) {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        if (!can_write(flags)) {
            return -EACCES;
        }
        idx = dir_create(n83, ATTR_ARCHIVE);
        if (idx < 0) {
            return idx;
        }
        err = dir_read((u32)idx, ent);
        if (err != 0) {
            return err;
        }
    } else if (idx < 0) {
        return idx;
    } else {
        if (ent[11] & ATTR_DIR) {
            return -EINVAL;
        }
        if (can_write(flags) && (ent[11] & ATTR_RDONLY)) {
            return -EACCES;
        }
        /* One writer, or any number of readers -- but not both. */
        if (can_write(flags) && entry_is_open((u32)idx)) {
            return -EBUSY;
        }
    }

    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->flags = (u8)flags;
    f->dir_index = (u32)idx;
    f->first = le16(&ent[26]);
    f->size = le32(&ent[28]);
    f->cur = f->first;
    f->cur_index = 0;

    if ((flags & O_TRUNC) && can_write(flags) && f->size != 0) {
        err = fat_free_chain(f->first);
        if (err != 0) {
            f->used = 0;
            return err;
        }
        f->first = 0;
        f->size = 0;
        f->cur = 0;
        f->dirty = 1;
        err = file_sync(f);
        if (err != 0) {
            f->used = 0;
            return err;
        }
    }

    f->pos = (flags & O_APPEND) ? f->size : 0;
    return fd;
}

static int fat_handle_close(int fd)
{
    struct fat_file *f = handle(fd);
    int err;

    if (!f) {
        return -EBADF;
    }
    err = file_sync(f);
    f->used = 0;
    if (err != 0) {
        return err;
    }
    return fat_flush_all();
}


static s32 fat_handle_read(int fd, void *buf, u32 len)
{
    struct fat_file *f = handle(fd);
    u8 *out = buf;
    u32 done = 0;

    if (!f) {
        return -EBADF;
    }
    if (!can_read(f->flags)) {
        return -EACCES;
    }

    while (done < len && f->pos < f->size) {
        u32 cl, lba, off, n, avail;
        int err = chain_seek(f, f->pos / cluster_bytes, 0, &cl);

        if (err < 0) {
            return err;
        }
        if (err == 1) {
            break;              /* the chain is shorter than the size */
        }

        off = f->pos % cluster_bytes;
        lba = cluster_lba(cl) + off / bytes_per_sector;
        off = off % bytes_per_sector;

        err = sb_get(lba);
        if (err != 0) {
            return err;
        }

        n = bytes_per_sector - off;
        avail = f->size - f->pos;
        if (n > avail) {
            n = avail;
        }
        if (n > len - done) {
            n = len - done;
        }

        memcpy(out + done, &sb.data[off], n);
        f->pos += n;
        done += n;
    }
    return (s32)done;
}

static s32 fat_handle_write(int fd, const void *buf, u32 len)
{
    struct fat_file *f = handle(fd);
    const u8 *in = buf;
    u32 done = 0;

    if (!f) {
        return -EBADF;
    }
    if (!can_write(f->flags)) {
        return -EACCES;
    }
    if (f->flags & O_APPEND) {
        f->pos = f->size;
    }

    while (done < len) {
        u32 cl, lba, off, n;
        int err = chain_seek(f, f->pos / cluster_bytes, 1, &cl);

        if (err < 0) {
            /* Out of space partway through is a short write, not a
             * failure -- what was written is already on the disk. */
            if (err == -ENOSPC && done > 0) {
                break;
            }
            return err;
        }

        off = f->pos % cluster_bytes;
        lba = cluster_lba(cl) + off / bytes_per_sector;
        off = off % bytes_per_sector;

        n = bytes_per_sector - off;
        if (n > len - done) {
            n = len - done;
        }

        /*
         * Read the sector first unless the write covers all of it and
         * lands beyond the end of the file, where there is nothing to
         * preserve.
         */
        if (n == bytes_per_sector) {
            err = sb_flush();
            if (err != 0) {
                return err;
            }
            sb.lba = lba;
            sb.valid = 1;
        } else {
            err = sb_get(lba);
            if (err != 0) {
                return err;
            }
        }

        memcpy(&sb.data[off], in + done, n);
        sb.dirty = 1;

        f->pos += n;
        done += n;
        if (f->pos > f->size) {
            f->size = f->pos;
        }
        f->dirty = 1;
    }

    /* Terminate the chain: chain_seek() may have appended clusters. */
    if (f->dirty && cluster_valid(f->cur)) {
        u16 next;
        int err = fat_get(f->cur, &next);

        if (err == 0 && next == 0) {
            err = fat_set(f->cur, 0xffff);
        }
        if (err != 0) {
            return err;
        }
    }

    return (s32)done;
}

static s32 fat_handle_seek(int fd, s32 offset, int whence)
{
    struct fat_file *f = handle(fd);
    s32 base;

    if (!f) {
        return -EBADF;
    }
    switch (whence) {
    case SEEK_SET: base = 0;            break;
    case SEEK_CUR: base = (s32)f->pos;  break;
    case SEEK_END: base = (s32)f->size; break;
    default:       return -EINVAL;
    }
    if (base + offset < 0) {
        return -EINVAL;
    }
    f->pos = (u32)(base + offset);
    return (s32)f->pos;
}



/* ---------------------------------------------------------------- */
/* Directory operations                                              */
/* ---------------------------------------------------------------- */

static int fat_unlink(const char *name)
{
    char n83[11];
    u8 ent[DIRENT_SIZE];
    int idx, err;
    u32 first;

    if (!mounted) {
        return -ENODEV;
    }
    err = name_to_83(name, n83);
    if (err != 0) {
        return err;
    }
    idx = dir_lookup(n83, ent);
    if (idx < 0) {
        return idx;
    }
    if (entry_is_open((u32)idx)) {
        return -EBUSY;
    }
    if (ent[11] & ATTR_RDONLY) {
        return -EACCES;
    }

    first = le16(&ent[26]);
    if (first != 0) {
        err = fat_free_chain(first);
        if (err != 0) {
            return err;
        }
    }

    /*
     * MS-DOS marks the entry deleted by overwriting the first byte of
     * the name with 0xE5 and leaves the rest alone.  Zeroing the first
     * cluster as well keeps a later scan from following a chain that has
     * just been freed.
     */
    ent[0] = 0xe5;
    put_le16(&ent[26], 0);
    err = dir_write((u32)idx, ent);
    if (err != 0) {
        return err;
    }
    return fat_flush_all();
}

static int fat_rename(const char *from, const char *to)
{
    char f83[11], t83[11];
    u8 ent[DIRENT_SIZE];
    int idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = name_to_83(from, f83);
    if (err != 0) {
        return err;
    }
    err = name_to_83(to, t83);
    if (err != 0) {
        return err;
    }
    if (memcmp(f83, t83, 11) == 0) {
        return 0;
    }

    if (dir_lookup(t83, 0) >= 0) {
        return -EEXIST;
    }

    idx = dir_lookup(f83, ent);
    if (idx < 0) {
        return idx;
    }
    if (entry_is_open((u32)idx)) {
        return -EBUSY;
    }

    /* The name is the only thing that moves; cluster chain and size stay
     * exactly where they are. */
    memcpy(ent, t83, 11);
    err = dir_write((u32)idx, ent);
    if (err != 0) {
        return err;
    }
    return fat_flush_all();
}

static void fill_dirent(const u8 *ent, struct dirent *out)
{
    struct tm t;

    name_from_83(ent, out->d_name);
    out->d_size = le32(&ent[28]);
    out->d_mode = (ent[11] & ATTR_DIR) ? S_IFDIR : S_IFREG;
    out->d_mode |= S_IRUSR;
    if (!(ent[11] & ATTR_RDONLY)) {
        out->d_mode |= S_IWUSR;
    }
    fat_to_tm(le16(&ent[24]), le16(&ent[22]), &t);
    out->d_mtime = timegm(&t);
}

static int fat_lookup_dirent(const char *name, struct dirent *out)
{
    char n83[11];
    u8 ent[DIRENT_SIZE];
    int idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = name_to_83(name, n83);
    if (err != 0) {
        return err;
    }
    idx = dir_lookup(n83, ent);
    if (idx < 0) {
        return idx;
    }
    fill_dirent(ent, out);
    return 0;
}

static int fat_stat(const char *name, struct stat *st)
{
    struct dirent de;
    int err = fat_lookup_dirent(name, &de);

    if (err < 0) {
        return err;
    }
    st->st_mode = de.d_mode;
    st->st_size = de.d_size;
    st->st_mtime = de.d_mtime;
    st->st_blocks = cluster_bytes ?
                    (de.d_size + cluster_bytes - 1) / cluster_bytes : 0;
    return 0;
}

static int fat_readdir(int index, struct dirent *out)
{
    u32 i;
    int seen = 0;
    u8 ent[DIRENT_SIZE];

    if (!mounted) {
        return -ENODEV;
    }
    if (index < 0) {
        return -EINVAL;
    }

    for (i = 0; i < root_entries; i++) {
        int err = dir_read(i, ent);

        if (err != 0) {
            return err;
        }
        if (ent[0] == 0x00) {
            break;
        }
        if (!dir_entry_is_file(ent)) {
            continue;
        }
        if (seen == index) {
            fill_dirent(ent, out);
            return 0;
        }
        seen++;
    }
    return -ENOENT;
}

/* ---------------------------------------------------------------- */
/* The filesystem as the VFS sees it                                 */
/*                                                                    */
/* Below this line nothing is FAT-specific in shape: the same five    */
/* file operations and the same nine filesystem operations would be   */
/* implemented by any other filesystem, which is the point.           */
/* ---------------------------------------------------------------- */

/*
 * A descriptor's priv holds the internal handle, biased by one so that
 * handle 0 is distinguishable from a NULL priv.
 */
static int priv_to_handle(struct file *f)
{
    return (int)(u32)f->priv - 1;
}

static s32 fat_file_read(struct file *f, void *buf, u32 len)
{
    return fat_handle_read(priv_to_handle(f), buf, len);
}

static s32 fat_file_write(struct file *f, const void *buf, u32 len)
{
    return fat_handle_write(priv_to_handle(f), buf, len);
}

static s32 fat_file_lseek(struct file *f, s32 offset, int whence)
{
    return fat_handle_seek(priv_to_handle(f), offset, whence);
}

static int fat_file_close(struct file *f)
{
    return fat_handle_close(priv_to_handle(f));
}

static const struct file_ops fat_file_ops = {
    fat_file_read,
    fat_file_write,
    fat_file_lseek,
    0,                          /* no ioctl on a regular file */
    fat_file_close
};

static int fat_open(const char *path, int flags, struct file *f)
{
    int h = fat_handle_open(path, flags);

    if (h < 0) {
        return h;
    }
    f->ops = &fat_file_ops;
    f->priv = (void *)(u32)(h + 1);
    f->flags = flags;
    return 0;
}

static int fat_mount(struct blockdev *b)
{
    int err;

    if (!b || !b->read) {
        return -ENXIO;
    }
    if (b->sector_size != SECTOR_SIZE) {
        return -EINVAL;
    }
    dev = b;
    err = fat_mount_dev();
    if (err < 0) {
        dev = 0;
    }
    return err;
}

static int fat_umount(void)
{
    mounted = 0;
    dev = 0;
    return 0;
}

static int fat_statfs(struct statfs *s)
{
    int i;

    if (!mounted) {
        return -ENODEV;
    }
    s->f_bsize = fat_cluster_bytes();
    s->f_blocks = fat_total_bytes() / (cluster_bytes ? cluster_bytes : 1);
    s->f_bfree = fat_free_bytes() / (cluster_bytes ? cluster_bytes : 1);
    s->f_type = "fat16";
    for (i = 0; i < 11; i++) {
        s->f_label[i] = fat_label()[i];
        if (!s->f_label[i]) {
            break;
        }
    }
    s->f_label[11] = '\0';
    return 0;
}

static int fat_sync(void)
{
    if (!mounted) {
        return 0;
    }
    return fat_flush_all();
}

static struct fs_type fat16_type = {
    "fat16",
    fat_mount,
    fat_umount,
    fat_open,
    fat_unlink,
    fat_rename,
    fat_stat,
    fat_readdir,
    fat_statfs,
    fat_sync,
    0
};

int fat16_init(void)
{
    return vfs_register(&fat16_type);
}
