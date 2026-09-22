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
#define FAT_MAX_OPEN   128     /* the whole machine, not per task */
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
    struct timeval tv;
    struct tm now;

    /* The same clock time() reads, so a file just written is never
     * stamped later than the time a program then asks for. */
    clock_get(&tv);
    if (tv.tv_sec > 0) {
        gmtime_r((time_t)tv.tv_sec, &now);
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

static void name_from_83(const u8 raw[11], char out[NAME_MAX + 1]);

/*
 * The short name as a program sees it. Byte 12 of an entry has two bits
 * Windows NT added and Linux honours: the base, or the extension, was
 * all lower case when the file was made, and is shown that way -- so a
 * file another system called "readme.txt" without needing a long name
 * does not come back as README.TXT.
 */
/*
 * A short name's bytes above 127 are in the volume's OEM code page,
 * which on a PC is 437 -- Linux's vfat default, and the code page of the
 * font this machine's screen draws with. As UTF-8, like every other name.
 */
static const u16 cp437_high[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
    0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
    0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
    0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
    0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
    0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
    0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

static int utf16_to_utf8(const u16 *in, int n, char *out, int size);

static void short_name(const u8 *ent, char out[NAME_MAX + 1])
{
    char *dot;
    char *c;

    name_from_83(ent, out);
    dot = 0;
    for (c = out; *c; c++) {
        if (*c == '.') {
            dot = c;
        }
    }
    for (c = out; *c; c++) {
        int in_ext = dot && c > dot;

        if (((ent[12] & 0x08) && !in_ext && c != dot) ||
            ((ent[12] & 0x10) && in_ext)) {
            if (*c >= 'A' && *c <= 'Z') {
                *c = (char)(*c - 'A' + 'a');
            }
        }
    }

    /* Then the OEM bytes, if there are any, to UTF-8. */
    for (c = out; *c; c++) {
        if ((u8)*c >= 0x80) {
            u16 units[13];
            int n = 0;

            for (c = out; *c && n < 12; c++) {
                units[n++] = ((u8)*c >= 0x80) ? cp437_high[(u8)*c - 0x80]
                                              : (u16)(u8)*c;
            }
            utf16_to_utf8(units, n, out, NAME_MAX + 1);
            break;
        }
    }
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

/*
 * A DIRECTORY IS ONE OF TWO THINGS on FAT16, and that is the whole
 * reason this layer needs generalising rather than extending.
 *
 * The root directory is a fixed run of sectors with a fixed size, laid
 * down when the volume was made and unable to grow. Every other
 * directory is an ordinary cluster chain, exactly like a file, whose
 * contents happen to be directory entries. FAT32 abolished the
 * distinction by making the root a chain too; FAT16 did not, so both
 * cases have to be carried.
 *
 * `cluster` of zero means the root. Everything below takes one of these
 * rather than assuming.
 */
struct dir {
    u32 cluster;                /* 0 = the fixed root directory */
};

static const struct dir ROOT_DIR = { 0 };

/* How many entries a directory can hold. The root's is fixed; a chain
 * grows, so this walks it. */
static u32 dir_entries(const struct dir *d)
{
    u32 n = 0, cl;
    u16 next;

    if (!d->cluster) {
        return root_entries;
    }
    for (cl = d->cluster; cluster_valid(cl); ) {
        n += cluster_bytes / DIRENT_SIZE;
        if (fat_get(cl, &next) != 0) {
            break;
        }
        cl = next;
    }
    return n;
}

/* The sector holding entry `index` of this directory. */
static int dir_entry_lba_of(const struct dir *d, u32 index, u32 *lba)
{
    u32 per_sector = bytes_per_sector / DIRENT_SIZE;

    if (!d->cluster) {
        if (index >= root_entries) {
            return -ENOENT;
        }
        *lba = root_start + index / per_sector;
        return 0;
    }

    {
        u32 per_cluster = cluster_bytes / DIRENT_SIZE;
        u32 skip = index / per_cluster;
        u32 within = index % per_cluster;
        u32 cl = d->cluster;
        u16 next;

        while (skip-- > 0) {
            if (!cluster_valid(cl) || fat_get(cl, &next) != 0) {
                return -ENOENT;
            }
            cl = next;
        }
        if (!cluster_valid(cl)) {
            return -ENOENT;
        }
        *lba = cluster_lba(cl) + within / per_sector;
        return 0;
    }
}

static u32 dir_entry_off(u32 index)
{
    return (index % (bytes_per_sector / DIRENT_SIZE)) * DIRENT_SIZE;
}

/*
 * Fill a cluster with zeroes.
 *
 * A fresh directory cluster must be blank, because a zero first byte is
 * what says "no entry has ever been used beyond here" and every scan
 * stops there. A cluster holding whatever the last file left behind
 * would be read as a directory full of rubbish.
 */
static int zero_cluster(u32 cl)
{
    u32 lba = cluster_lba(cl);
    u32 i;

    for (i = 0; i < sectors_per_cluster; i++) {
        int err = sb_get(lba + i);

        if (err != 0) {
            return err;
        }
        memset(sb.data, 0, bytes_per_sector);
        sb.dirty = 1;
    }
    return 0;
}

static int dir_read_in(const struct dir *d, u32 index, u8 *ent)
{
    u32 lba;
    int err = dir_entry_lba_of(d, index, &lba);

    if (err != 0) {
        return err;
    }
    err = sb_get(lba);
    if (err != 0) {
        return err;
    }
    memcpy(ent, &sb.data[dir_entry_off(index)], DIRENT_SIZE);
    return 0;
}

static int dir_read(u32 index, u8 *ent)
{
    return dir_read_in(&ROOT_DIR, index, ent);
}

static int dir_write_in(const struct dir *d, u32 index, const u8 *ent)
{
    u32 lba;
    int err = dir_entry_lba_of(d, index, &lba);

    if (err != 0) {
        return err;
    }
    err = sb_get(lba);
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

/* Find name83 in `d`. Returns the entry index, or -ENOENT. */
static int dir_lookup_in(const struct dir *d, const char name83[11],
                         u8 *ent_out)
{
    u32 i, n = dir_entries(d);
    u8 ent[DIRENT_SIZE];

    for (i = 0; i < n; i++) {
        int err = dir_read_in(d, i, ent);

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


/* --- long names ------------------------------------------------------ */

/*
 * VFAT long names.
 *
 * A long name is a run of extra directory entries in front of an
 * ordinary 8.3 entry, each carrying 13 UTF-16 code units, marked with
 * the attribute byte 0x0F that no real file can have -- which is how
 * DOS came to ignore them. They are stored last piece first: the entry
 * furthest from the 8.3 one has the highest sequence number and the
 * 0x40 "last" bit, and each carries a checksum of the 8.3 name so that
 * a run left behind by something that knew nothing of long names is
 * recognisable as stale.
 *
 * Names cross the system call boundary as bytes, and those bytes are
 * taken to be UTF-8, which is what Linux does with vfat's utf8 option
 * and what a name made anywhere else will survive a round trip in.
 *
 * Comparison ignores case in ASCII only. Windows folds the whole of
 * the BMP through a table on the volume; ASCII is where nearly every
 * name actually differs only in case.
 */
#define LFN_UNITS_MAX   255
#define LFN_SLOTS_MAX   20              /* 20 * 13 = 260 >= 255 */

static u8 lfn_checksum(const u8 *n83)
{
    u8 sum = 0;
    int i;

    for (i = 0; i < 11; i++) {
        sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + n83[i]);
    }
    return sum;
}

/* The byte offsets of an LFN entry's 13 code units. */
static const u8 lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/*
 * UTF-8 to UTF-16. A byte that does not start a valid sequence is taken
 * as the Latin-1 character of the same value, so no name is refused
 * for its encoding and none is silently changed into another. Returns
 * the number of units, or -ENAMETOOLONG.
 */
static int utf8_to_utf16(const char *in, u16 *out, int max)
{
    const u8 *p = (const u8 *)in;
    int n = 0;

    while (*p) {
        u32 c = *p;
        int more = 0;

        if (c >= 0xf0 && c < 0xf8) { more = 3; c &= 0x07; }
        else if (c >= 0xe0)        { more = 2; c &= 0x0f; }
        else if (c >= 0xc0)        { more = 1; c &= 0x1f; }
        if (more && c < 0xf8) {
            int k;

            for (k = 1; k <= more; k++) {
                if ((p[k] & 0xc0) != 0x80) {
                    break;
                }
            }
            if (k > more) {
                for (k = 1; k <= more; k++) {
                    c = (c << 6) | (p[k] & 0x3f);
                }
                p += more + 1;
            } else {
                c = *p++;           /* not UTF-8: take it as Latin-1 */
            }
        } else {
            p++;
        }
        if (c >= 0x10000) {
            if (n + 2 > max) {
                return -ENAMETOOLONG;
            }
            c -= 0x10000;
            out[n++] = (u16)(0xd800 | (c >> 10));
            out[n++] = (u16)(0xdc00 | (c & 0x3ff));
        } else {
            if (n + 1 > max) {
                return -ENAMETOOLONG;
            }
            out[n++] = (u16)c;
        }
    }
    return n;
}

/* UTF-16 to UTF-8. Returns the length, or -1 if it would not fit. */
static int utf16_to_utf8(const u16 *in, int n, char *out, int size)
{
    int i, o = 0;

    for (i = 0; i < n; i++) {
        u32 c = in[i];

        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n &&
            in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (in[i + 1] - 0xdc00);
            i++;
        }
        if (c < 0x80) {
            if (o + 1 >= size) return -1;
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= size) return -1;
            out[o++] = (char)(0xc0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            if (o + 3 >= size) return -1;
            out[o++] = (char)(0xe0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        } else {
            if (o + 4 >= size) return -1;
            out[o++] = (char)(0xf0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[o] = '\0';
    return o;
}

/*
 * Collects a long name while a directory is scanned. Fed every entry in
 * order; after an 8.3 entry, lfn_name() says whether the run in front
 * of it was a complete, matching long name, and what it spelled.
 */
struct lfn_acc {
    int active;
    int next;                   /* the sequence number expected next  */
    int slots;                  /* how many the run said it would be  */
    u8  sum;
    u32 first;                  /* index of the run's first entry     */
    u16 units[LFN_SLOTS_MAX * 13];
};

static void lfn_feed(struct lfn_acc *a, const u8 *ent, u32 index)
{
    int seq = ent[0] & 0x1f, k;

    if (ent[0] == 0xe5 || ent[0] == 0x00) {
        a->active = 0;
        return;
    }
    if (ent[0] & 0x40) {
        if (seq < 1 || seq > LFN_SLOTS_MAX) {
            a->active = 0;
            return;
        }
        a->active = 1;
        a->slots = seq;
        a->sum = ent[13];
        a->first = index;
    } else if (!a->active || seq != a->next || ent[13] != a->sum) {
        a->active = 0;
        return;
    }
    for (k = 0; k < 13; k++) {
        a->units[(seq - 1) * 13 + k] = le16(&ent[lfn_off[k]]);
    }
    a->next = seq - 1;
}

/*
 * After the 8.3 entry `ent`: its long name, in UTF-8, if a complete run
 * with the right checksum preceded it. Returns 1 and sets *first to the
 * run's first slot; 0 if there is none (or it would not fit in a
 * name, in which case the 8.3 name stands in).
 */
static int lfn_name(struct lfn_acc *a, const u8 *ent, char *out,
                    u32 *first)
{
    int n;

    if (!a->active || a->next != 0 || a->sum != lfn_checksum(ent)) {
        a->active = 0;
        return 0;
    }
    a->active = 0;
    for (n = 0; n < a->slots * 13; n++) {
        if (a->units[n] == 0x0000) {
            break;
        }
    }
    if (utf16_to_utf8(a->units, n, out, NAME_MAX + 1) < 0) {
        return 0;
    }
    if (first) {
        *first = a->first;
    }
    return 1;
}

static int name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = upcase(*a), y = upcase(*b);

        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

/* Scratch for the scans below: bigger than belongs on a kernel stack,
 * and the filesystem is not reentrant anyway (sb, fb). */
static struct lfn_acc scan_acc;
static char scan_long[NAME_MAX + 1];
static char scan_short[NAME_MAX + 1];

/*
 * Find `name` in `d`, by its long name or its 8.3 name, ignoring case.
 * Returns the index of the 8.3 entry, and in *first the index of the
 * first entry belonging to the file -- its long-name run, or the 8.3
 * entry itself if it has none.
 */
static int dir_find(const struct dir *d, const char *name, u8 *ent_out,
                    u32 *first)
{
    u32 i, n = dir_entries(d);
    u8 ent[DIRENT_SIZE];

    scan_acc.active = 0;
    for (i = 0; i < n; i++) {
        u32 lfirst = i;
        int err = dir_read_in(d, i, ent);

        if (err != 0) {
            return err;
        }
        if (ent[0] == 0x00) {
            break;
        }
        if ((ent[11] & ATTR_LFN) == ATTR_LFN) {
            lfn_feed(&scan_acc, ent, i);
            continue;
        }
        if (!dir_entry_is_file(ent)) {
            scan_acc.active = 0;
            continue;
        }
        /* lfirst becomes the run's first slot if there is a run, and
         * stays i if not -- whichever of the two names matches, the
         * run belongs to the file. */
        if (lfn_name(&scan_acc, ent, scan_long, &lfirst) &&
            name_eq(scan_long, name)) {
            goto found;
        }
        short_name(ent, scan_short);
        if (name_eq(scan_short, name)) {
            goto found;
        }
        continue;
found:
        if (ent_out) {
            memcpy(ent_out, ent, DIRENT_SIZE);
        }
        if (first) {
            *first = lfirst;
        }
        return (int)i;
    }
    return -ENOENT;
}

/*
 * A name that is ALREADY an upper-case 8.3 name, in ASCII, gets an 8.3
 * entry and nothing more -- the way it always did, and what DOS and
 * every earlier image expect. Anything else keeps its spelling in a
 * long name.
 */
static int name_is_plain_83(const char *name, char n83[11])
{
    const char *p;

    if (name_to_83(name, n83) != 0) {
        return 0;
    }
    for (p = name; *p; p++) {
        if ((u8)*p >= 0x80 || (*p >= 'a' && *p <= 'z')) {
            return 0;
        }
    }
    return 1;
}

static int lfn_char_ok(char c)
{
    if ((u8)c < 0x20) {
        return 0;
    }
    switch (c) {
    case '"': case '*': case '/': case ':': case '<': case '>':
    case '?': case '\\': case '|':
        return 0;
    default:
        return 1;
    }
}

/*
 * An 8.3 alias for a long name, unique in `d`: the first six usable
 * characters, upper-cased, then ~1, ~2 and so on, and the first three
 * of the extension -- the scheme Windows uses, so that DOS, and anything
 * reading only 8.3 names, sees something recognisable.
 */
static int make_alias(const struct dir *d, const char *name, char n83[11])
{
    char base[9], ext[4];
    const char *dot = 0, *p;
    int nb = 0, ne = 0;
    u32 num;

    /*
     * A name that would be a legal 8.3 name but for its case --
     * "readme.txt" -- has itself, upper-cased, as its alias, with no ~1,
     * if that is free. Windows and Linux both do this, and it is what
     * DOS then sees.
     */
    for (p = name; *p && (u8)*p < 0x80; p++) {
    }
    if (!*p && name_to_83(name, n83) == 0 &&
        dir_lookup_in(d, n83, 0) == -ENOENT) {
        return 0;
    }

    for (p = name; *p; p++) {
        if (*p == '.' && p != name) {
            dot = p;
        }
    }
    for (p = name; *p && p != dot && nb < 8; p++) {
        u8 c = (u8)*p;

        if (c == ' ' || c == '.' || (c >= 0x80 && c < 0xc0)) {
            continue;               /* spaces, dots, UTF-8 tails: dropped */
        }
        base[nb++] = (c >= 0x80 || !name_char_ok((char)c)) ? '_' : upcase((char)c);
    }
    if (nb == 0) {
        base[nb++] = '_';
    }
    for (p = dot ? dot + 1 : ""; *p && ne < 3; p++) {
        u8 c = (u8)*p;

        if (c == ' ' || c == '.' || (c >= 0x80 && c < 0xc0)) {
            continue;
        }
        ext[ne++] = (c >= 0x80 || !name_char_ok((char)c)) ? '_' : upcase((char)c);
    }

    for (num = 1; num < 1000000; num++) {
        char digits[8];
        int nd = 0, keep, i;
        u32 v = num;

        while (v) {
            digits[nd++] = (char)('0' + v % 10);
            v /= 10;
        }
        keep = 8 - 1 - nd;
        if (keep > nb) {
            keep = nb;
        }
        memset(n83, ' ', 11);
        for (i = 0; i < keep; i++) {
            n83[i] = base[i];
        }
        n83[i++] = '~';
        while (nd) {
            n83[i++] = digits[--nd];
        }
        for (i = 0; i < ne; i++) {
            n83[8 + i] = ext[i];
        }
        if (dir_lookup_in(d, n83, 0) == -ENOENT) {
            return 0;
        }
    }
    return -EEXIST;
}

static int dir_extend(const struct dir *d);
static int dir_create_in(const struct dir *d, const char name83[11], u8 attr);

/*
 * `count` consecutive free slots in `d`, growing a subdirectory if it
 * has to. Returns the first one's index.
 */
static int dir_find_free_run(const struct dir *d, u32 count)
{
    for (;;) {
        u32 i, n = dir_entries(d), run = 0, start = 0;
        u8 ent[DIRENT_SIZE];
        int err;

        for (i = 0; i < n; i++) {
            err = dir_read_in(d, i, ent);
            if (err != 0) {
                return err;
            }
            if (ent[0] == 0x00 || ent[0] == 0xe5) {
                if (run == 0) {
                    start = i;
                }
                if (++run == count) {
                    return (int)start;
                }
            } else {
                run = 0;
            }
        }
        err = dir_extend(d);
        if (err != 0) {
            return err;
        }
    }
}

/*
 * Make an entry called `name` in `d`: 8.3 alone if the name is one,
 * otherwise a long-name run and an alias. Returns the index of the 8.3
 * entry, which is what everything else addresses a file by.
 */
static int dir_create_named(const struct dir *d, const char *name, u8 attr)
{
    static u16 units[LFN_UNITS_MAX + 1];
    char n83[11];
    u8 ent[DIRENT_SIZE];
    u16 date, time;
    const char *p;
    int nu, k, start, j, err;
    u8 sum;

    if (name_is_plain_83(name, n83)) {
        return dir_create_in(d, n83, attr);
    }
    for (p = name; *p; p++) {
        if (!lfn_char_ok(*p)) {
            return -EINVAL;
        }
    }
    nu = utf8_to_utf16(name, units, LFN_UNITS_MAX);
    if (nu < 0) {
        return nu;
    }
    if (nu == 0) {
        return -EINVAL;
    }
    err = make_alias(d, name, n83);
    if (err != 0) {
        return err;
    }
    sum = lfn_checksum((const u8 *)n83);
    k = (nu + 12) / 13;
    start = dir_find_free_run(d, (u32)k + 1);
    if (start < 0) {
        return start;
    }

    for (j = 0; j < k; j++) {
        int seq = k - j, c;

        memset(ent, 0, DIRENT_SIZE);
        ent[0] = (u8)(seq | (j == 0 ? 0x40 : 0));
        ent[11] = ATTR_LFN;
        ent[13] = sum;
        for (c = 0; c < 13; c++) {
            int u = (seq - 1) * 13 + c;
            u16 v = (u < nu) ? units[u] : (u == nu ? 0x0000 : 0xffff);

            put_le16(&ent[lfn_off[c]], v);
        }
        err = dir_write_in(d, (u32)(start + j), ent);
        if (err != 0) {
            return err;
        }
    }

    memset(ent, 0, DIRENT_SIZE);
    memcpy(ent, n83, 11);
    ent[11] = attr;
    fs_now(&date, &time);
    put_le16(&ent[14], time);
    put_le16(&ent[16], date);
    put_le16(&ent[18], date);
    put_le16(&ent[22], time);
    put_le16(&ent[24], date);
    err = dir_write_in(d, (u32)(start + k), ent);
    if (err != 0) {
        return err;
    }
    return start + k;
}

/* Mark a file's long-name run, from `first` up to its 8.3 entry, free. */
static int lfn_delete(const struct dir *d, u32 first, u32 idx)
{
    u8 ent[DIRENT_SIZE];
    u32 i;
    int err;

    for (i = first; i < idx; i++) {
        err = dir_read_in(d, i, ent);
        if (err != 0) {
            return err;
        }
        ent[0] = 0xe5;
        err = dir_write_in(d, i, ent);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

/*
 * Trailing dots and spaces are not part of a name on a FAT volume --
 * Windows drops them and so does Linux's vfat -- except in "." and "..".
 */
static void name_trim(char *name)
{
    int n = (int)strlen(name);

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return;
    }
    while (n > 0 && (name[n - 1] == '.' || name[n - 1] == ' ')) {
        name[--n] = '\0';
    }
}

/* --- paths ----------------------------------------------------------- */

/*
 * Walk a path down to the directory that holds its last component, and
 * hand back that component.
 *
 * "/etc/rc.local" gives the directory /etc and the name "rc.local".
 * "notes.txt" gives the current directory and the same name. An empty
 * last component -- "/etc/" -- gives the directory and nothing, which is
 * how a caller asks to open a directory rather than a file in one.
 *
 * Relative paths are resolved against `start`, which is the calling
 * task's working directory. Every absolute path begins at the root, and
 * nothing here ever looks above it: ".." in the root is the root, which
 * is what every Unix does and what stops a path escaping the volume.
 */
static int path_walk(const struct dir *start, const char *path,
                     struct dir *out_dir, char *out_name, int *out_last_is_dir)
{
    struct dir d = *start;
    const char *p = path;

    if (out_last_is_dir) {
        *out_last_is_dir = 0;
    }
    out_name[0] = '\0';

    if (*p == '/') {
        d = ROOT_DIR;
        while (*p == '/') {
            p++;
        }
    }

    for (;;) {
        char comp[NAME_MAX + 1];
        const char *slash = p;
        u32 n = 0;
        u8 ent[DIRENT_SIZE];
        int idx;

        while (*slash && *slash != '/') {
            slash++;
        }
        n = (u32)(slash - p);
        if (n == 0) {
            /* Trailing slash, or the path was just "/". */
            *out_dir = d;
            if (out_last_is_dir) {
                *out_last_is_dir = 1;
            }
            return 0;
        }
        if (n >= sizeof(comp)) {
            return -ENAMETOOLONG;
        }
        memcpy(comp, p, n);
        comp[n] = '\0';
        name_trim(comp);
        if (!comp[0]) {
            return -ENOENT;     /* nothing but dots and spaces */
        }

        if (!*slash) {
            /*
             * "." and ".." are directories even as the last component,
             * and they are not 8.3 names -- name_to_83 rejects them,
             * which is right for a file and wrong here. `cd ..` is the
             * case that matters and it failed outright until this.
             */
            if (comp[0] == '.' && comp[1] == '\0') {
                *out_dir = d;
                if (out_last_is_dir) {
                    *out_last_is_dir = 1;
                }
                return 0;
            }
            if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
                if (d.cluster) {
                    static const char dd[11] = {
                        '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
                    };

                    idx = dir_lookup_in(&d, dd, ent);
                    if (idx < 0) {
                        return -ENOENT;
                    }
                    d.cluster = le16(&ent[26]);
                }
                *out_dir = d;
                if (out_last_is_dir) {
                    *out_last_is_dir = 1;
                }
                return 0;
            }

            /* The last component: this is the name the caller wants,
             * long or short -- dir_find and dir_create_named decide
             * which it is. */
            strcpy(out_name, comp);
            *out_dir = d;
            return 0;
        }

        /* An intermediate component has to be a directory. */
        if (comp[0] == '.' && comp[1] == '\0') {
            /* stay where we are */
        } else if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (d.cluster) {
                /* The 8.3 form of "..", space padded and NOT
                 * NUL terminated -- a directory entry's name is
                 * eleven bytes with no terminator at all. */
                static const char dotdot[11] = {
                    '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
                };

                idx = dir_lookup_in(&d, dotdot, ent);
                if (idx < 0) {
                    return -ENOENT;
                }
                d.cluster = le16(&ent[26]);
            }
            /* ".." in the root is the root. */
        } else {
            idx = dir_find(&d, comp, ent, 0);
            if (idx < 0) {
                return -ENOENT;
            }
            if (!(ent[11] & ATTR_DIR)) {
                return -ENOTDIR;
            }
            d.cluster = le16(&ent[26]);
        }

        p = slash;
        while (*p == '/') {
            p++;
        }
    }
}

/*
 * The directory a task is working in.
 *
 * The STORAGE is in `struct task`, reached through vfs_cwd_*(), because
 * a working directory belongs to a task exactly the way its descriptors
 * do. The MEANING stays here: the task layer holds a u32 and does not
 * know it is a cluster number.
 *
 * It used to be the two statics below this comment, which meant a
 * chdir() in any task moved every other task's idea of where it was --
 * including the shell's. Nothing had caught it because the shell was
 * the only thing that ever called chdir.
 */
static struct dir cwd_of_current(void)
{
    struct dir d;

    d.cluster = vfs_cwd_ino();
    return d;
}

static int fat_chdir(const char *path)
{
    struct dir cwd = cwd_of_current();
    char cwd_path[PATH_MAX];
    struct dir d;
    char name[NAME_MAX + 1];
    int last_is_dir;
    int err;
    u8 ent[DIRENT_SIZE];
    int idx;

    strncpy(cwd_path, vfs_cwd_path(), sizeof(cwd_path) - 1);
    cwd_path[sizeof(cwd_path) - 1] = '\0';

    err = path_walk(&cwd, path, &d, name, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir) {
        cwd = d;
    } else {
        idx = dir_find(&d, name, ent, 0);
        if (idx < 0) {
            return -ENOENT;
        }
        if (!(ent[11] & ATTR_DIR)) {
            return -ENOTDIR;
        }
        d.cluster = le16(&ent[26]);
        cwd = d;
    }

    /* Keep a printable form for `pwd`. Rebuilt rather than walked back
     * up, because ".." only gives the parent's cluster and not its
     * name. */
    if (path[0] == '/') {
        strncpy(cwd_path, path, sizeof(cwd_path) - 1);
        cwd_path[sizeof(cwd_path) - 1] = '\0';
    } else if (strcmp(path, "..") == 0) {
        int n = (int)strlen(cwd_path);

        while (n > 1 && cwd_path[n - 1] != '/') {
            n--;
        }
        if (n > 1) {
            n--;
        }
        cwd_path[n] = '\0';
        if (!cwd_path[0]) {
            strcpy(cwd_path, "/");
        }
    } else if (strcmp(path, ".") != 0) {
        u32 n = (u32)strlen(cwd_path);

        if (n > 1 && n + 1 < sizeof(cwd_path)) {
            cwd_path[n++] = '/';
        }
        strncpy(cwd_path + n, path, sizeof(cwd_path) - n - 1);
        cwd_path[sizeof(cwd_path) - 1] = '\0';
    }
    if (!cwd.cluster) {
        strcpy(cwd_path, "/");
    }

    vfs_cwd_set(cwd.cluster, cwd_path);
    return 0;
}

static const char *fat_getcwd(void)
{
    return vfs_cwd_path();
}

/*
 * Claim a free slot in `d` and write a fresh entry into it.
 *
 * A subdirectory can GROW, and this is where that happens: if every
 * slot is taken, another cluster is chained on and its entries become
 * free slots. The root cannot, which is the one place FAT16's two kinds
 * of directory behave differently to a caller -- a full root is full
 * for good, and always was.
 */
static int dir_create_in(const struct dir *d, const char name83[11], u8 attr)
{
    u32 i, n = dir_entries(d);
    u8 ent[DIRENT_SIZE];
    u16 date, time;

    for (i = 0; i < n; i++) {
        int err = dir_read_in(d, i, ent);

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

        err = dir_write_in(d, i, ent);
        if (err != 0) {
            return err;
        }
        return (int)i;
    }

    /* Full. The first slot of a new cluster is the one we wanted. */
    {
        int err = dir_extend(d);

        if (err != 0) {
            return err;
        }
    }
    return dir_create_in(d, name83, attr);
}

/*
 * Another cluster on the end of a directory, zeroed so that every entry
 * in it is free. A chained directory can grow; the root cannot.
 */
static int dir_extend(const struct dir *d)
{
    u32 last = d->cluster, add;
    u16 next;
    int err;

    if (!d->cluster) {
        return -ENOSPC;
    }
    while (cluster_valid(last)) {
        if (fat_get(last, &next) != 0) {
            return -EIO;
        }
        if (!cluster_valid(next)) {
            break;
        }
        last = next;
    }
    err = fat_alloc(&add);
    if (err != 0) {
        return err;
    }
    err = zero_cluster(add);
    if (err != 0) {
        return err;
    }
    if (fat_set(last, (u16)add) != 0) {
        return -EIO;
    }
    return 0;
}

/*
 * The clean-unmount flag.
 *
 * Byte 0x25 of the boot sector, reserved in the original BPB, is where
 * Linux keeps a "dirty" bit, and it is the one fsck.fat reads. That is
 * the one this kernel sets while the volume is mounted and clears when
 * it is unmounted.
 *
 * FAT[1]'s top bit is Windows 95's version of the same thing, and it is
 * READ -- a volume Windows left dirty is checked -- and SET when the
 * volume is clean, but never cleared. Clearing it makes FAT[1] stop
 * looking like an end-of-chain value, and mtools then refuses the whole
 * FAT ("Error reading FAT"), which would lock the host's tools out of
 * the disk for as long as the machine ran and after every crash. That
 * is exactly what the first version of this did.
 */
#define FAT1_CLEAN      0x8000u
#define BOOT_DIRTY_OFF  0x25
#define BOOT_DIRTY      0x01

static int volume_was_dirty;

static int boot_flag_get(u8 *flag)
{
    u8 bpb[SECTOR_SIZE];

    if (dev->read(dev, part_lba, 1, bpb) != 0) {
        return -EIO;
    }
    *flag = bpb[BOOT_DIRTY_OFF];
    return 0;
}

static int set_clean(int clean)
{
    u8 bpb[SECTOR_SIZE];
    u16 f1;
    int err;

    if (clean) {
        err = fat_get(1, &f1);
        if (err != 0) {
            return err;
        }
        if (!(f1 & FAT1_CLEAN)) {
            err = fat_set(1, (u16)(f1 | FAT1_CLEAN));
            if (err != 0) {
                return err;
            }
            err = fb_flush();
            if (err != 0) {
                return err;
            }
        }
    }
    if (dev->read(dev, part_lba, 1, bpb) != 0) {
        return -EIO;
    }
    if (clean) {
        bpb[BOOT_DIRTY_OFF] &= (u8)~BOOT_DIRTY;
    } else {
        bpb[BOOT_DIRTY_OFF] |= BOOT_DIRTY;
    }
    if (dev->write(dev, part_lba, 1, bpb) != 0) {
        return -EIO;
    }
    return 0;
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

    /* Was it put away properly last time? Then say it is in use now, so
     * that a crash from here on is noticed at the next mount. */
    {
        u16 f1 = FAT1_CLEAN;
        u8 bf = 0;

        if (fat_get(1, &f1) == 0 && boot_flag_get(&bf) == 0) {
            volume_was_dirty = !(f1 & FAT1_CLEAN) || (bf & BOOT_DIRTY);
            set_clean(0);
        }
    }
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

/*
 * An open FILE, shared by every descriptor open on it -- what an inode
 * is to Linux. The size and the chain live here and nowhere else, so a
 * writer and a reader of the same file see one file. They used to live
 * in each handle, which is why a file could not be opened for writing
 * while anything had it open for reading: a truncating writer would
 * have left the reader holding a size and a chain that no longer
 * existed. An editor that keeps a file open to lock it, then saves over
 * it, needs exactly that.
 */
struct fat_node {
    u8  used;
    u8  dirty;                  /* directory entry needs rewriting    */
    int refs;                   /* handles open on it                 */
    struct dir dir;             /* which directory holds its entry    */
    u32 dir_index;
    u32 first;                  /* first cluster, 0 if empty          */
    u32 size;
};

/* An open handle: a position, and a hint of where in the chain it is. */
struct fat_file {
    u8  used;
    u8  flags;
    struct fat_node *node;
    u32 pos;
    u32 cur;                    /* cluster holding cur_index          */
    u32 cur_index;              /* its position in the chain          */
};

static struct fat_node nodes[FAT_MAX_OPEN];
static struct fat_file files[FAT_MAX_OPEN];

/* Every handle's chain hint for `n` forgotten: the chain has shrunk
 * under them, and a hint can point at a cluster that is now free. */
static void node_forget_hints(struct fat_node *n)
{
    int i;

    for (i = 0; i < FAT_MAX_OPEN; i++) {
        if (files[i].used && files[i].node == n) {
            files[i].cur = 0;
            files[i].cur_index = 0;
        }
    }
}

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
    struct fat_node *n = f->node;
    u8 ent[DIRENT_SIZE];
    u16 date, time;
    int err;

    if (!n->dirty) {
        return 0;
    }
    err = dir_read_in(&n->dir, n->dir_index, ent);
    if (err != 0) {
        return err;
    }
    fs_now(&date, &time);
    put_le16(&ent[26], (u16)n->first);
    put_le32(&ent[28], n->size);
    put_le16(&ent[22], time);
    put_le16(&ent[24], date);
    ent[11] |= ATTR_ARCHIVE;
    err = dir_write_in(&n->dir, n->dir_index, ent);
    if (err != 0) {
        return err;
    }
    n->dirty = 0;
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
    struct fat_node *n = f->node;

    if (n->first == 0) {
        u32 c;
        int err;

        if (!alloc) {
            return 1;
        }
        err = fat_alloc(&c);
        if (err != 0) {
            return err;
        }
        n->first = c;
        f->cur = c;
        f->cur_index = 0;
        n->dirty = 1;
    }

    if (!cluster_valid(f->cur) || want < f->cur_index) {
        f->cur = n->first;
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
 * deleted or renamed while something holds it. The directory as well as
 * the slot: it compared the slot alone, so a file open in one directory
 * made every file at the same position in every other directory busy. */
static struct fat_node *node_of(const struct dir *d, u32 dir_index)
{
    int i;

    for (i = 0; i < FAT_MAX_OPEN; i++) {
        if (nodes[i].used && nodes[i].dir_index == dir_index &&
            nodes[i].dir.cluster == d->cluster) {
            return &nodes[i];
        }
    }
    return 0;
}

static int entry_is_open(const struct dir *d, u32 dir_index)
{
    return node_of(d, dir_index) != 0;
}

static int fat_handle_close(int fd);

static int fat_handle_open(const char *name, int flags)
{
    struct dir cwd = cwd_of_current();
    char nm[NAME_MAX + 1];
    u8 ent[DIRENT_SIZE];
    struct fat_file *f;
    struct dir open_dir;
    int fd, idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    {
        struct dir d;
        int last_is_dir;

        err = path_walk(&cwd, name, &d, nm, &last_is_dir);
        if (err != 0) {
            return err;
        }
        if (last_is_dir) {
            return -EISDIR;     /* "/etc/" is a directory, not a file */
        }
        open_dir = d;
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

    idx = dir_find(&open_dir, nm, ent, 0);
    if (idx == -ENOENT) {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        if (!can_write(flags)) {
            return -EACCES;
        }
        idx = dir_create_named(&open_dir, nm, ATTR_ARCHIVE);
        if (idx < 0) {
            return idx;
        }
        err = dir_read_in(&open_dir, (u32)idx, ent);
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
    }

    /* The file's node: the one already open on it, or a new one. */
    {
        struct fat_node *n = node_of(&open_dir, (u32)idx);
        int k;

        if (!n) {
            for (k = 0; k < FAT_MAX_OPEN && nodes[k].used; k++) {
            }
            if (k == FAT_MAX_OPEN) {
                return -ENFILE;
            }
            n = &nodes[k];
            memset(n, 0, sizeof(*n));
            n->used = 1;
            n->dir = open_dir;
            n->dir_index = (u32)idx;
            n->first = le16(&ent[26]);
            n->size = le32(&ent[28]);
        }
        n->refs++;

        memset(f, 0, sizeof(*f));
        f->used = 1;
        f->flags = (u8)flags;
        f->node = n;
        f->cur = n->first;
        f->cur_index = 0;
    }

    if ((flags & O_TRUNC) && can_write(flags) && f->node->size != 0) {
        struct fat_node *n = f->node;

        err = fat_free_chain(n->first);
        if (err == 0) {
            n->first = 0;
            n->size = 0;
            n->dirty = 1;
            node_forget_hints(n);
            err = file_sync(f);
        }
        if (err != 0) {
            fat_handle_close(fd);
            return err;
        }
    }

    f->pos = (flags & O_APPEND) ? f->node->size : 0;
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
    if (--f->node->refs <= 0) {
        f->node->used = 0;
    }
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

    while (done < len && f->pos < f->node->size) {
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
        avail = f->node->size - f->pos;
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
        f->pos = f->node->size;
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
        if (f->pos > f->node->size) {
            f->node->size = f->pos;
        }
        f->node->dirty = 1;
    }

    /* Terminate the chain: chain_seek() may have appended clusters. */
    if (f->node->dirty && cluster_valid(f->cur)) {
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
    case SEEK_END: base = (s32)f->node->size; break;
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

/* Delete the file at slot `idx` of `d`, whose entry is `ent`. */
static int delete_entry(const struct dir *d, int idx, u8 *ent)
{
    u32 first = le16(&ent[26]);
    int err;

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
    return dir_write_in(d, (u32)idx, ent);
}

/*
 * Through path_walk, like everything else. It used to take the name
 * with name_to_83 and look it up in the ROOT -- so a path with a slash
 * in it could not be unlinked, and a relative name in a subdirectory
 * found the root's file of that name -- and then write the deletion
 * through a struct dir that was never initialised, into whichever
 * directory the stack happened to name.
 */
static int fat_unlink(const char *name)
{
    struct dir cwd = cwd_of_current();
    struct dir target;
    char nm[NAME_MAX + 1];
    u8 ent[DIRENT_SIZE];
    u32 first;
    int idx, err, last_is_dir;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, name, &target, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir) {
        return -EISDIR;
    }
    idx = dir_find(&target, nm, ent, &first);
    if (idx < 0) {
        return idx;
    }
    if (ent[11] & ATTR_DIR) {
        return -EISDIR;         /* rmdir is for those, as on Linux */
    }
    if (entry_is_open(&target, (u32)idx)) {
        return -EBUSY;
    }
    if (ent[11] & ATTR_RDONLY) {
        return -EACCES;
    }
    err = lfn_delete(&target, first, (u32)idx);
    if (err != 0) {
        return err;
    }
    err = delete_entry(&target, idx, ent);
    if (err != 0) {
        return err;
    }
    return fat_flush_all();
}

/*
 * Is directory `c` the directory `anc`, or somewhere below it? Walks up
 * from `c` through the ".." entries. Moving a directory into itself or
 * into one of its own children would cut it off from the tree.
 */
static int dir_is_within(u32 c, u32 anc)
{
    int depth;

    for (depth = 0; depth < 64; depth++) {
        struct dir d;
        u8 e[DIRENT_SIZE];

        if (c == anc) {
            return 1;
        }
        if (c == 0) {
            return 0;
        }
        d.cluster = c;
        if (dir_read_in(&d, 1, e) != 0 || e[0] != '.' || e[1] != '.') {
            return 0;           /* no ".." where it should be: stop */
        }
        c = le16(&e[26]);
    }
    return 1;                   /* deeper than anything real: refuse */
}

/*
 * POSIX's rename, which an editor saving a file depends on: it writes
 * the new text to a temporary and renames it over the old. So:
 *
 *   - An existing file at the destination is REPLACED, not refused.
 *   - A move between directories really moves: a new entry in the
 *     destination, the old one deleted. A directory that moves has its
 *     ".." rewritten to its new parent.
 *
 * It used to look both names up in the root whatever the paths said,
 * write the new name into the source's directory at the slot the root
 * lookup found, and refuse any destination that existed.
 */
static int fat_rename(const char *from, const char *to)
{
    struct dir cwd = cwd_of_current();
    struct dir fdir, tdir;
    char fname[NAME_MAX + 1], tname[NAME_MAX + 1];
    u8 ent[DIRENT_SIZE], tent[DIRENT_SIZE];
    u32 ffirst, tfirst;
    int idx, tidx, err, last;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, from, &fdir, fname, &last);
    if (err != 0) {
        return err;
    }
    if (last) {
        return -EBUSY;          /* "/" or "dir/": not something to move */
    }
    err = path_walk(&cwd, to, &tdir, tname, &last);
    if (err != 0) {
        return err;
    }
    if (last) {
        return -EISDIR;
    }

    idx = dir_find(&fdir, fname, ent, &ffirst);
    if (idx < 0) {
        return idx;
    }
    if (fdir.cluster == tdir.cluster && strcmp(fname, tname) == 0) {
        return 0;               /* renaming something to itself */
    }
    if (entry_is_open(&fdir, (u32)idx)) {
        return -EBUSY;
    }
    if ((ent[11] & ATTR_DIR) && dir_is_within(tdir.cluster, le16(&ent[26]))) {
        return -EINVAL;
    }

    /*
     * Something already there -- unless it IS the source, found again
     * because only the case differs ("readme" to "README"), which is a
     * rename like any other.
     */
    tidx = dir_find(&tdir, tname, tent, &tfirst);
    if (tidx >= 0 && !(tdir.cluster == fdir.cluster && tidx == idx)) {
        if (tent[11] & ATTR_DIR) {
            return (ent[11] & ATTR_DIR) ? -EEXIST : -EISDIR;
        }
        if (ent[11] & ATTR_DIR) {
            return -ENOTDIR;
        }
        if (entry_is_open(&tdir, (u32)tidx)) {
            return -EBUSY;
        }
        if (tent[11] & ATTR_RDONLY) {
            return -EACCES;
        }
        err = lfn_delete(&tdir, tfirst, (u32)tidx);
        if (err == 0) {
            err = delete_entry(&tdir, tidx, tent);
        }
        if (err != 0) {
            return err;
        }
    }

    /*
     * The quick case: same directory, an 8.3 name with no long name
     * going to another plain 8.3 name. Only the eleven bytes change,
     * and the entry keeps its slot.
     */
    {
        char t83[11];

        if (fdir.cluster == tdir.cluster && ffirst == (u32)idx &&
            name_is_plain_83(tname, t83)) {
            memcpy(ent, t83, 11);
            err = dir_write_in(&fdir, (u32)idx, ent);
            if (err != 0) {
                return err;
            }
            return fat_flush_all();
        }
    }

    /*
     * Everything else: a new entry, with a long name if it needs one,
     * holding the same cluster chain, size and times; then the old
     * entry and its long name freed -- without freeing the chain, which
     * belongs to the new one now. New first, so that running out of
     * room loses nothing.
     */
    tidx = dir_create_named(&tdir, tname, ent[11]);
    if (tidx < 0) {
        return tidx;
    }
    err = dir_read_in(&tdir, (u32)tidx, tent);
    if (err != 0) {
        return err;
    }
    memcpy(tent + 11, ent + 11, DIRENT_SIZE - 11);  /* all but the name */
    err = dir_write_in(&tdir, (u32)tidx, tent);
    if (err != 0) {
        return err;
    }
    err = lfn_delete(&fdir, ffirst, (u32)idx);
    if (err != 0) {
        return err;
    }
    ent[0] = 0xe5;
    err = dir_write_in(&fdir, (u32)idx, ent);
    if (err != 0) {
        return err;
    }
    if ((tent[11] & ATTR_DIR) && fdir.cluster != tdir.cluster) {
        struct dir self;
        u8 dd[DIRENT_SIZE];

        self.cluster = le16(&tent[26]);
        if (dir_read_in(&self, 1, dd) == 0 && dd[0] == '.' && dd[1] == '.') {
            put_le16(&dd[26], (u16)tdir.cluster);
            err = dir_write_in(&self, 1, dd);
            if (err != 0) {
                return err;
            }
        }
    }
    return fat_flush_all();
}

/*
 * An inode number for an entry. FAT has none, and some programs --
 * anything that asks whether two names are the same file, and every
 * readdir that skips a zero d_ino as a deleted slot -- need one that is
 * nonzero, stable, and the same from stat() as from readdir().
 *
 * A directory is its first cluster, which it keeps for life (the root,
 * which has no cluster, is 1). A file is where its entry sits: the
 * directory's cluster, plus one, above the slot number. Clusters stop
 * below 65,525 and slots below 65,536, so the two ranges never meet --
 * though renaming a file moves its entry, and so changes its number.
 */
static u32 ino_for(const struct dir *parent, u32 idx, const u8 *ent)
{
    if (ent[11] & ATTR_DIR) {
        u32 c = le16(&ent[26]);

        return c ? c : 1;
    }
    return ((parent->cluster + 1) << 16) | (idx & 0xffffUL);
}

static void fill_dirent(const u8 *ent, struct dirent *out)
{
    struct tm t;

    short_name(ent, out->d_name);
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
    struct dir cwd = cwd_of_current();
    struct dir d;
    char nm[NAME_MAX + 1];
    u8 ent[DIRENT_SIZE];
    int idx, err, last_is_dir;

    if (!mounted) {
        return -ENODEV;
    }

    /*
     * Through path_walk, not name_to_83: stat() is asked about "/etc/rc"
     * as often as about "notes.txt", and looking only in the current
     * directory made every absolute path report that it did not exist.
     */
    err = path_walk(&cwd, name, &d, nm, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir) {
        /* A path ending in a slash, or "/" itself: a directory, and
         * there is no entry anywhere describing the root. */
        memset(out, 0, sizeof(*out));
        out->d_mode = S_IFDIR | 0755;
        out->d_ino = d.cluster ? d.cluster : 1;
        return 0;
    }

    idx = dir_find(&d, nm, ent, 0);
    if (idx < 0) {
        return idx;
    }
    fill_dirent(ent, out);
    out->d_ino = ino_for(&d, (u32)idx, ent);
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
    st->st_ino = de.d_ino;
    st->st_blocks = cluster_bytes ?
                    (de.d_size + cluster_bytes - 1) / cluster_bytes : 0;
    return 0;
}

/*
 * Entry `index` of the directory whose identity is `ino` -- its first
 * cluster, 0 for the root, the same u32 the VFS keeps for a working
 * directory. Counting from the start each time is quadratic in the
 * size of a directory, and FAT directories are small.
 */
static int fat_readdir_in(u32 ino, int index, struct dirent *out)
{
    struct dir d;
    u32 i;
    int seen = 0;
    u8 ent[DIRENT_SIZE];

    if (!mounted) {
        return -ENODEV;
    }
    if (index < 0) {
        return -EINVAL;
    }
    d.cluster = ino;
    scan_acc.active = 0;

    for (i = 0; i < dir_entries(&d); i++) {
        int err = dir_read_in(&d, i, ent);

        if (err != 0) {
            return err;
        }
        if (ent[0] == 0x00) {
            break;
        }
        if ((ent[11] & ATTR_LFN) == ATTR_LFN) {
            lfn_feed(&scan_acc, ent, i);
            continue;
        }
        if (!dir_entry_is_file(ent)) {
            scan_acc.active = 0;
            continue;
        }
        if (seen == index) {
            fill_dirent(ent, out);
            if (lfn_name(&scan_acc, ent, scan_long, 0)) {
                strcpy(out->d_name, scan_long);
            }
            out->d_ino = ino_for(&d, i, ent);
            return 0;
        }
        scan_acc.active = 0;
        seen++;
    }
    return -ENOENT;
}

static int fat_readdir(int index, struct dirent *out)
{
    return fat_readdir_in(vfs_cwd_ino(), index, out);
}

/*
 * The absolute path of the directory whose identity is `ino`, worked out
 * now rather than remembered: every directory but the root holds ".."
 * as its second entry, naming its parent's cluster, and the parent holds
 * the one entry whose first cluster is this directory -- which gives
 * its name. Repeat to the root. It is the classic Unix getcwd, and it
 * is what makes a directory descriptor follow its directory through a
 * rename, as Linux's does. -ENOENT if the directory has been removed.
 */
static int fat_dir_path(u32 ino, char *out, u32 size)
{
    char buf[PATH_MAX];
    u32 pos = sizeof(buf) - 1;
    u32 cur = ino;
    int depth = 0;

    if (!mounted) {
        return -ENODEV;
    }
    buf[pos] = '\0';
    while (cur != 0) {
        struct dir d, parent;
        u8 ent[DIRENT_SIZE];
        char name[NAME_MAX + 1];
        u32 i, n;
        int found = 0, err;

        if (++depth > PATH_MAX / 2) {
            return -ELOOP;          /* a corrupt volume, not a deep one */
        }
        d.cluster = cur;
        err = dir_read_in(&d, 1, ent);
        if (err != 0) {
            return err;
        }
        if (memcmp(ent, "..         ", 11) != 0 || !(ent[11] & ATTR_DIR)) {
            return -EIO;
        }
        parent.cluster = le16(&ent[26]);

        scan_acc.active = 0;
        for (i = 0; i < dir_entries(&parent); i++) {
            err = dir_read_in(&parent, i, ent);
            if (err != 0) {
                return err;
            }
            if (ent[0] == 0x00) {
                break;
            }
            if ((ent[11] & ATTR_LFN) == ATTR_LFN) {
                lfn_feed(&scan_acc, ent, i);
                continue;
            }
            if (dir_entry_is_file(ent) && (ent[11] & ATTR_DIR) &&
                ent[0] != '.' && le16(&ent[26]) == cur) {
                if (!lfn_name(&scan_acc, ent, name, 0)) {
                    short_name(ent, name);
                }
                found = 1;
                break;
            }
            scan_acc.active = 0;
        }
        if (!found) {
            return -ENOENT;
        }
        n = (u32)strlen(name);
        if (n + 1 > pos) {
            return -ENAMETOOLONG;
        }
        pos -= n;
        memcpy(buf + pos, name, n);
        buf[--pos] = '/';
        cur = parent.cluster;
    }
    if (buf[pos] == '\0') {
        buf[--pos] = '/';
    }
    if (sizeof(buf) - 1 - pos + 1 > size) {
        return -ENAMETOOLONG;
    }
    strcpy(out, buf + pos);
    return 0;
}

/* The identity of the directory `path` names, for fat_readdir_in. */
static int fat_dir_ino(const char *path, u32 *ino)
{
    struct dir cwd = cwd_of_current();
    struct dir d;
    char name[NAME_MAX + 1];
    u8 ent[DIRENT_SIZE];
    int last_is_dir, idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, path, &d, name, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (!last_is_dir) {
        idx = dir_find(&d, name, ent, 0);
        if (idx < 0) {
            return idx;
        }
        if (!(ent[11] & ATTR_DIR)) {
            return -ENOTDIR;
        }
        d.cluster = le16(&ent[26]);
    }
    *ino = d.cluster;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Checking the volume                                               */
/* ---------------------------------------------------------------- */

/*
 * The check. Everything reachable from the root is walked, every cluster
 * it reaches is marked, and then everything else is looked at:
 *
 *   - both FAT copies say the same thing;
 *   - every chain runs through valid, allocated clusters and ends;
 *   - no cluster belongs to two chains, and no chain loops;
 *   - a file's size and its chain agree;
 *   - a directory's "." and ".." point where they should;
 *   - every long-name run belongs to the 8.3 entry after it;
 *   - no cluster is allocated that nothing reaches.
 *
 * With `repair`, each is put right the way fsck.fat puts it right:
 * chains cut at the first bad link (or where they meet another), sizes
 * made to fit the chain, orphaned long names and lost clusters freed,
 * the first FAT copied over the second.
 */
static u8 seen[65536 / 8];

static int seen_test(u32 c)
{
    return (seen[c >> 3] >> (c & 7)) & 1;
}

static void seen_set(u32 c)
{
    seen[c >> 3] |= (u8)(1 << (c & 7));
}

struct walk_dir {
    u32 cluster;
    u32 parent;
};

#define FSCK_DEPTH 64

/*
 * Walk one chain from `first`, marking what it reaches. Returns its
 * length in clusters. A cluster is bad if it is out of range, already
 * reached (a cross-link, or a loop back into the chain itself), free,
 * or marked bad; the chain is cut at the cluster before it -- or, if
 * the very first is bad, *cut_first says so and the entry has to lose
 * its chain altogether.
 */
static int fsck_chain(u32 first, int repair, struct fsck_report *r,
                      int *cut_first)
{
    u32 c = first, prev = 0;
    int len = 0;

    *cut_first = 0;
    for (;;) {
        u16 v = 0;
        int bad = !cluster_valid(c);

        if (!bad && seen_test(c)) {
            r->cross_linked++;
            bad = 2;
        }
        if (!bad) {
            if (fat_get(c, &v) != 0) {
                return -EIO;
            }
            if (v == 0 || v == FAT_BAD) {
                bad = 1;
            }
        }
        if (bad) {
            if (bad == 1) {
                r->bad_chains++;
            }
            if (!prev) {
                *cut_first = 1;
            } else if (repair) {
                fat_set(prev, 0xffff);
            }
            if (repair) {
                r->fixed++;
            }
            return len;
        }
        seen_set(c);
        len++;
        if (v >= FAT_EOC) {
            return len;
        }
        prev = c;
        c = v;
    }
}

/* Unmark a chain cut off a file, so the lost-cluster pass frees it. */
static void unmark_chain(u32 c)
{
    while (cluster_valid(c) && seen_test(c)) {
        u16 next;

        seen[c >> 3] &= (u8)~(1 << (c & 7));
        if (fat_get(c, &next) != 0) {
            return;
        }
        c = next;
    }
}

static int fsck_dir(const struct walk_dir *w, struct walk_dir *stack,
                    int *top, int repair, struct fsck_report *r)
{
    struct dir d;
    u32 i, n, run_first = 0, run_len = 0, owner_first = 0;
    u8 ent[DIRENT_SIZE];
    int err;

    d.cluster = w->cluster;
    n = dir_entries(&d);
    scan_acc.active = 0;

    for (i = 0; i < n; i++) {
        err = dir_read_in(&d, i, ent);
        if (err != 0) {
            return err;
        }
        if (ent[0] == 0x00) {
            break;
        }
        if ((ent[11] & ATTR_LFN) == ATTR_LFN && ent[0] != 0xe5) {
            if (run_len == 0 || (ent[0] & 0x40)) {
                if (run_len) {
                    goto orphan;    /* a new run began: the old one lost */
                }
                run_first = i;
                run_len = 0;
            }
            run_len++;
            lfn_feed(&scan_acc, ent, i);
            continue;
        }
        if (run_len && (ent[0] == 0xe5 || !dir_entry_is_file(ent) ||
                        !lfn_name(&scan_acc, ent, scan_long, 0))) {
            /* A long-name run with no owner after it. */
orphan:
            r->orphan_lfn += run_len;
            if (repair) {
                u32 k;
                u8 e[DIRENT_SIZE];

                for (k = run_first; k < run_first + run_len; k++) {
                    if (dir_read_in(&d, k, e) == 0) {
                        e[0] = 0xe5;
                        dir_write_in(&d, k, e);
                    }
                }
                r->fixed++;
            }
            run_len = 0;
            scan_acc.active = 0;
            if ((ent[11] & ATTR_LFN) == ATTR_LFN && ent[0] != 0xe5) {
                run_first = i;      /* the entry that began a new run */
                run_len = 1;
                lfn_feed(&scan_acc, ent, i);
                continue;
            }
        }
        owner_first = run_len ? run_first : i;  /* its own run, if any */
        run_len = 0;
        scan_acc.active = 0;
        if (!dir_entry_is_file(ent)) {
            continue;
        }

        if (ent[0] == '.' && ent[1] == ' ') {           /* "." */
            if (d.cluster && le16(&ent[26]) != d.cluster) {
                r->dot_entries++;
                if (repair) {
                    put_le16(&ent[26], (u16)d.cluster);
                    dir_write_in(&d, i, ent);
                    r->fixed++;
                }
            }
            continue;
        }
        if (ent[0] == '.' && ent[1] == '.' && ent[2] == ' ') { /* ".." */
            if (d.cluster && le16(&ent[26]) != w->parent) {
                r->dot_entries++;
                if (repair) {
                    put_le16(&ent[26], (u16)w->parent);
                    dir_write_in(&d, i, ent);
                    r->fixed++;
                }
            }
            continue;
        }

        {
            u32 first = le16(&ent[26]);
            u32 size = le32(&ent[28]);
            int cut = 0, len = 0;

            if (first) {
                len = fsck_chain(first, repair, r, &cut);
                if (len < 0) {
                    return len;
                }
            }
            if (ent[11] & ATTR_DIR) {
                r->dirs++;
                if (!first || cut) {
                    /* A directory with no clusters of its own cannot be
                     * entered; nothing can be saved from it. */
                    r->bad_chains++;
                    if (repair) {
                        lfn_delete(&d, owner_first, i);
                        ent[0] = 0xe5;
                        dir_write_in(&d, i, ent);
                        r->fixed++;
                    }
                    continue;
                }
                if (*top < FSCK_DEPTH) {
                    stack[*top].cluster = first;
                    stack[*top].parent = d.cluster;
                    (*top)++;
                } else {
                    r->too_deep++;
                }
                continue;
            }

            r->files++;
            {
                u32 have, need;
                int changed = 0;

                if (cut) {
                    len = 0;
                    if (repair) {
                        put_le16(&ent[26], 0);
                        first = 0;
                        changed = 1;
                    }
                }
                have = (u32)len * cluster_bytes;
                need = (size + cluster_bytes - 1) / cluster_bytes;

                /*
                 * The size has to fit the chain. Claiming more than the
                 * chain holds: the size is cut to what is there, which
                 * keeps every byte that really exists (fsck.fat's rule).
                 * A chain longer than the size needs: the extra clusters
                 * are cut off and freed.
                 */
                if (size > have) {
                    r->size_fixed++;
                    if (repair) {
                        size = have;
                        changed = 1;
                    }
                } else if ((u32)len > need) {
                    r->size_fixed++;
                    if (repair) {
                        if (need == 0) {
                            unmark_chain(first);
                            put_le16(&ent[26], 0);
                        } else {
                            u32 c = first, k;
                            u16 next = 0;

                            for (k = 1; k < need; k++) {
                                fat_get(c, &next);
                                c = next;
                            }
                            fat_get(c, &next);
                            fat_set(c, 0xffff);
                            unmark_chain(next);
                        }
                        changed = 1;
                    }
                }
                if (changed) {
                    put_le32(&ent[28], size);
                    dir_write_in(&d, i, ent);
                    r->fixed++;
                }
            }
        }
    }
    if (run_len) {
        r->orphan_lfn += run_len;   /* a run at the very end */
        if (repair) {
            u32 k;
            u8 e[DIRENT_SIZE];

            for (k = run_first; k < run_first + run_len; k++) {
                if (dir_read_in(&d, k, e) == 0) {
                    e[0] = 0xe5;
                    dir_write_in(&d, k, e);
                }
            }
            r->fixed++;
        }
    }
    return 0;
}

static int fat_check(int flags, struct fsck_report *r)
{
    struct walk_dir stack[FSCK_DEPTH];
    int top = 0, err, repair = flags & FSCK_REPAIR, i;
    u32 c, s;

    memset(r, 0, sizeof(*r));
    if (!mounted) {
        return -ENODEV;
    }
    r->was_dirty = volume_was_dirty;
    if ((flags & FSCK_IF_DIRTY) && !volume_was_dirty) {
        return 0;
    }
    /* Repairing a file under something that has it open would pull its
     * chain out from under the handle. */
    if (repair) {
        for (i = 0; i < FAT_MAX_OPEN; i++) {
            if (files[i].used) {
                return -EBUSY;
            }
        }
    }
    err = fat_flush_all();
    if (err != 0) {
        return err;
    }
    memset(seen, 0, sizeof(seen));
    r->cluster_bytes = cluster_bytes;

    /* The FAT copies, straight off the disk. */
    if (num_fats > 1) {
        static u8 a[SECTOR_SIZE], b[SECTOR_SIZE];

        for (s = 0; s < sectors_per_fat; s++) {
            if (dev->read(dev, fat_start + s, 1, a) != 0 ||
                dev->read(dev, fat_start + sectors_per_fat + s, 1, b) != 0) {
                return -EIO;
            }
            /* FAT[1]'s clean bit is not a disagreement worth reporting:
             * the copies are written together, but compare the rest. */
            if (s == 0) {
                b[3] = a[3];
            }
            if (memcmp(a, b, SECTOR_SIZE) != 0) {
                r->fat_mismatch++;
            }
        }
    }

    stack[top].cluster = 0;
    stack[top].parent = 0;
    top++;
    while (top > 0) {
        struct walk_dir w = stack[--top];

        err = fsck_dir(&w, stack, &top, repair, r);
        if (err != 0) {
            return err;
        }
    }

    for (c = 2; c < total_clusters + 2; c++) {
        u16 v;

        if (fat_get(c, &v) != 0) {
            return -EIO;
        }
        if (v != 0 && v != FAT_BAD && !seen_test(c)) {
            r->lost_clusters++;
            if (repair) {
                fat_set(c, 0);
                v = 0;
            }
        }
        if (v == 0) {
            r->clusters_free++;
        } else if (v != FAT_BAD) {
            r->clusters_used++;
        }
    }
    if (repair && r->lost_clusters) {
        r->fixed++;
    }

    err = fat_flush_all();
    if (err != 0) {
        return err;
    }

    /* The second copy made the same as the first, which fb_flush has
     * kept every write the check made in step with. */
    if (repair && r->fat_mismatch && num_fats > 1) {
        static u8 a[SECTOR_SIZE];

        for (s = 0; s < sectors_per_fat; s++) {
            if (dev->read(dev, fat_start + s, 1, a) != 0 ||
                dev->write(dev, fat_start + sectors_per_fat + s, 1, a) != 0) {
                return -EIO;
            }
        }
        r->fixed++;
        fb.valid = 0;
    }
    if (repair) {
        volume_was_dirty = 0;
        /* Checked and put right: clean as far as Windows' flag goes. The
         * boot-sector flag stays set until unmount -- the volume is in
         * use. */
        {
            u16 f1;

            if (fat_get(1, &f1) == 0 && !(f1 & FAT1_CLEAN)) {
                fat_set(1, (u16)(f1 | FAT1_CLEAN));
                fat_flush_all();
            }
        }
    }
    return 0;
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


/*
 * Describe an open file. The size and the time come from the in-memory
 * handle rather than from the directory entry on disk, because a file
 * that has been written and not yet flushed is longer than its entry
 * says -- and a program that stats its own descriptor to find out how
 * much it wrote deserves the true answer.
 */
static int fat_file_fstat(struct file *f, struct stat *st)
{
    /*
     * Through handle(), like every other file operation. This used to
     * read f->priv as a pointer to the handle, when it is the handle's
     * NUMBER plus one -- so fstat read its "size" out of the vector
     * table, which is never zero, and the only test asked for a size
     * greater than zero.
     */
    struct fat_file *ff = handle(priv_to_handle(f));

    if (!ff) {
        return -EBADF;
    }
    st->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
    st->st_ino = ((ff->node->dir.cluster + 1) << 16) |
                 (ff->node->dir_index & 0xffffUL);
    st->st_size = ff->node->size;
    st->st_mtime = 0;
    st->st_blocks = cluster_bytes
                    ? (ff->node->size + cluster_bytes - 1) / cluster_bytes : 0;
    return 0;
}

/*
 * ftruncate. Shorter: the clusters past the new end are freed, and every
 * handle open on the file forgets where it was in the chain. Longer: the
 * gap is written with zeroes, through the same path a write takes, so
 * that nothing beyond the old end reads back as whatever the clusters
 * held before -- which is what POSIX says a file extended this way
 * contains.
 */
static int fat_file_truncate(struct file *f, u32 len)
{
    struct fat_file *ff = handle(priv_to_handle(f));
    struct fat_node *n;
    int err;

    if (!ff) {
        return -EBADF;
    }
    if (!can_write(ff->flags)) {
        return -EINVAL;             /* Linux's answer for a read-only fd */
    }
    n = ff->node;

    if (len < n->size) {
        u32 need = (len + cluster_bytes - 1) / cluster_bytes;

        if (need == 0) {
            if (n->first) {
                err = fat_free_chain(n->first);
                if (err != 0) {
                    return err;
                }
            }
            n->first = 0;
        } else {
            u32 c = n->first, k;
            u16 next = 0;

            for (k = 1; k < need; k++) {
                if (fat_get(c, &next) != 0) {
                    return -EIO;
                }
                c = next;
            }
            if (fat_get(c, &next) != 0) {
                return -EIO;
            }
            err = fat_set(c, 0xffff);
            if (err == 0 && cluster_valid(next)) {
                err = fat_free_chain(next);
            }
            if (err != 0) {
                return err;
            }
        }
        n->size = len;
        n->dirty = 1;
        node_forget_hints(n);
        err = file_sync(ff);
        return err ? err : fat_flush_all();
    }

    if (len > n->size) {
        static const u8 zeroes[SECTOR_SIZE];
        u32 pos = ff->pos;
        u8 flags = ff->flags;

        ff->flags &= (u8)~O_APPEND;
        ff->pos = n->size;
        while (ff->pos < len) {
            u32 k = len - ff->pos;
            s32 w;

            if (k > sizeof(zeroes)) {
                k = sizeof(zeroes);
            }
            w = fat_handle_write(priv_to_handle(f), zeroes, k);
            if (w <= 0) {
                ff->pos = pos;
                ff->flags = flags;
                return w < 0 ? w : -EIO;
            }
        }
        ff->pos = pos;
        ff->flags = flags;
        err = file_sync(ff);
        return err ? err : fat_flush_all();
    }
    return 0;
}

static const struct file_ops fat_file_ops = {
    fat_file_read,
    fat_file_write,
    fat_file_lseek,
    0,                          /* no ioctl on a regular file */
    fat_file_close,
    fat_file_fstat,
    0,                          /* poll: the default; see dev.h */
    fat_file_truncate,
    0,                          /* mmap: a file is mapped by mmap.c */
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
    if (mounted) {
        /* Everything out, then the flags that say it all got there. */
        fat_flush_all();
        set_clean(1);
    }
    mounted = 0;
    dev = 0;
    return 0;
}

/*
 * Make a directory.
 *
 * A new directory is a cluster containing exactly two entries, "." and
 * "..", and nothing else. They are not decoration: ".." is the ONLY
 * record of a directory's parent anywhere on the volume -- a FAT
 * directory entry says nothing about where it lives -- so a path walk
 * hitting ".." reads it from here. A directory made without them cannot
 * be left.
 *
 * The parent of a directory in the root is recorded as cluster 0, which
 * is how FAT spells "the root" and why the root itself needs no entry.
 */
static int fat_mkdir(const char *path)
{
    struct dir cwd = cwd_of_current();
    struct dir parent, self;
    char nm[NAME_MAX + 1];
    u8 ent[DIRENT_SIZE];
    u32 cl;
    u16 date, time;
    int idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, path, &parent, nm, 0);
    if (err != 0) {
        return err;
    }
    if (!nm[0] || strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0 ||
        dir_find(&parent, nm, 0, 0) >= 0) {
        return -EEXIST;
    }

    err = fat_alloc(&cl);
    if (err != 0) {
        return err;
    }
    err = zero_cluster(cl);
    if (err != 0) {
        return err;
    }

    idx = dir_create_named(&parent, nm, ATTR_DIR);
    if (idx < 0) {
        fat_free_chain(cl);
        return idx;
    }
    err = dir_read_in(&parent, (u32)idx, ent);
    if (err != 0) {
        return err;
    }
    put_le16(&ent[26], (u16)cl);
    err = dir_write_in(&parent, (u32)idx, ent);
    if (err != 0) {
        return err;
    }

    self.cluster = cl;
    fs_now(&date, &time);

    /* "." -> itself */
    memset(ent, 0, DIRENT_SIZE);
    memset(ent, ' ', 11);
    ent[0] = '.';
    ent[11] = ATTR_DIR;
    put_le16(&ent[22], time);
    put_le16(&ent[24], date);
    put_le16(&ent[26], (u16)cl);
    err = dir_write_in(&self, 0, ent);
    if (err != 0) {
        return err;
    }

    /* ".." -> the parent, or 0 meaning the root */
    memset(ent, 0, DIRENT_SIZE);
    memset(ent, ' ', 11);
    ent[0] = '.';
    ent[1] = '.';
    ent[11] = ATTR_DIR;
    put_le16(&ent[22], time);
    put_le16(&ent[24], date);
    put_le16(&ent[26], (u16)parent.cluster);
    err = dir_write_in(&self, 1, ent);
    if (err != 0) {
        return err;
    }

    return fat_flush_all();
}

/*
 * Remove one, but only if it is empty.
 *
 * Empty means nothing but "." and "..", which is why they have to be
 * skipped rather than counted. Removing a directory that still had
 * things in it would orphan every one of their cluster chains -- they
 * would be marked allocated forever with nothing pointing at them.
 */
static int fat_rmdir(const char *path)
{
    struct dir cwd = cwd_of_current();
    struct dir parent, self;
    char nm[NAME_MAX + 1];
    u8 ent[DIRENT_SIZE];
    u32 i, n, first;
    int idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, path, &parent, nm, 0);
    if (err != 0) {
        return err;
    }
    idx = dir_find(&parent, nm, ent, &first);
    if (idx < 0) {
        return idx;
    }
    if (!(ent[11] & ATTR_DIR)) {
        return -ENOTDIR;
    }

    self.cluster = le16(&ent[26]);
    if (self.cluster == cwd.cluster) {
        return -EBUSY;          /* somebody is standing in it */
    }

    n = dir_entries(&self);
    for (i = 0; i < n; i++) {
        u8 e[DIRENT_SIZE];

        if (dir_read_in(&self, i, e) != 0) {
            break;
        }
        if (e[0] == 0x00) {
            break;
        }
        if (e[0] == 0xe5 || (e[11] & ATTR_LFN) == ATTR_LFN) {
            continue;
        }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) {
            continue;           /* . and .. do not count */
        }
        return -ENOTEMPTY;
    }

    fat_free_chain(self.cluster);
    err = lfn_delete(&parent, first, (u32)idx);
    if (err != 0) {
        return err;
    }
    ent[0] = 0xe5;
    put_le16(&ent[26], 0);
    err = dir_write_in(&parent, (u32)idx, ent);
    if (err != 0) {
        return err;
    }
    return fat_flush_all();
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

/*
 * bmap: the sector holding byte `off` of an open file.
 *
 * Walking the chain from the start for every sector would make swapon,
 * which asks for every sector of the file in order, quadratic -- tens of
 * millions of FAT lookups for a swap file of a few megabytes. So the
 * last answer is remembered, and a question further along the same
 * file starts from there.
 */
static int fat_bmap(struct file *f, u32 off, u32 *lba, struct blockdev **bd)
{
    static const struct fat_node *last_node;
    static u32 last_first, last_index, last_cluster;
    struct fat_file *ff = handle(priv_to_handle(f));
    u32 index, cl, k;

    if (!ff) {
        return -EBADF;
    }
    if (off >= ff->node->size || !ff->node->first) {
        return -EINVAL;
    }
    index = off / cluster_bytes;
    /* The same node AND the same chain: a node is reused for another
     * file once this one is closed. */
    if (last_node == ff->node && last_first == ff->node->first &&
        last_index <= index) {
        k = last_index;
        cl = last_cluster;
    } else {
        k = 0;
        cl = ff->node->first;
    }
    for (; k < index; k++) {
        u16 next;

        if (fat_get(cl, &next) != 0 || !cluster_valid(next)) {
            last_node = 0;
            return -EIO;
        }
        cl = next;
    }
    last_node = ff->node;
    last_first = ff->node->first;
    last_index = index;
    last_cluster = cl;
    *lba = cluster_lba(cl) + (off % cluster_bytes) / 512;
    *bd = dev;
    return 0;
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
    fat_mkdir,
    fat_rmdir,
    fat_chdir,
    fat_getcwd,
    fat_readdir_in,
    fat_dir_ino,
    fat_dir_path,
    fat_check,
    fat_bmap,
    0
};

int fat16_init(void)
{
    return vfs_register(&fat16_type);
}
