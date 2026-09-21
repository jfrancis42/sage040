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

static int dir_lookup(const char name83[11], u8 *ent_out)
{
    return dir_lookup_in(&ROOT_DIR, name83, ent_out);
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
                     struct dir *out_dir, char out_name[11], int *out_last_is_dir)
{
    struct dir d = *start;
    const char *p = path;

    if (out_last_is_dir) {
        *out_last_is_dir = 0;
    }
    memset(out_name, ' ', 11);

    if (*p == '/') {
        d = ROOT_DIR;
        while (*p == '/') {
            p++;
        }
    }

    for (;;) {
        char comp[64];
        const char *slash = p;
        u32 n = 0;
        u8 ent[DIRENT_SIZE];
        char n83[11];
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

        if (!*slash) {
            int err;

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

            /* The last component: this is the name the caller wants. */
            err = name_to_83(comp, n83);
            if (err != 0) {
                return err;
            }
            memcpy(out_name, n83, 11);
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
            int err = name_to_83(comp, n83);

            if (err != 0) {
                return err;
            }
            idx = dir_lookup_in(&d, n83, ent);
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
 * One per task would live in `struct task`; it is here for now because
 * the filesystem is the only thing that knows what a directory IS, and
 * moving it out means giving the task layer a filesystem-independent
 * handle. That is the right shape and it is not needed until there is a
 * second filesystem.
 */
static struct dir cwd = { 0 };
static char cwd_path[PATH_MAX] = "/";

static int fat_chdir(const char *path)
{
    struct dir d;
    char name[11];
    int last_is_dir;
    int err = path_walk(&cwd, path, &d, name, &last_is_dir);
    u8 ent[DIRENT_SIZE];
    int idx;

    if (err != 0) {
        return err;
    }
    if (last_is_dir) {
        cwd = d;
    } else {
        idx = dir_lookup_in(&d, name, ent);
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
    return 0;
}

static const char *fat_getcwd(void)
{
    return cwd_path;
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

    /*
     * Full. A chained directory can be extended; the root cannot.
     */
    if (d->cluster) {
        u32 last = d->cluster, add;
        u16 next;
        int err;

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
        /* The first slot of the new cluster is the one we wanted. */
        return dir_create_in(d, name83, attr);
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
    struct dir dir;             /* which directory holds its entry    */
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
    err = dir_read_in(&f->dir, f->dir_index, ent);
    if (err != 0) {
        return err;
    }
    fs_now(&date, &time);
    put_le16(&ent[26], (u16)f->first);
    put_le32(&ent[28], f->size);
    put_le16(&ent[22], time);
    put_le16(&ent[24], date);
    ent[11] |= ATTR_ARCHIVE;
    err = dir_write_in(&f->dir, f->dir_index, ent);
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
    struct dir open_dir;
    int fd, idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    {
        struct dir d;
        int last_is_dir;

        err = path_walk(&cwd, name, &d, n83, &last_is_dir);
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

    idx = dir_lookup_in(&open_dir, n83, ent);
    if (idx == -ENOENT) {
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }
        if (!can_write(flags)) {
            return -EACCES;
        }
        idx = dir_create_in(&open_dir, n83, ATTR_ARCHIVE);
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
        /* One writer, or any number of readers -- but not both. */
        if (can_write(flags) && entry_is_open((u32)idx)) {
            return -EBUSY;
        }
    }

    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->flags = (u8)flags;
    /*
     * AFTER the memset, which is where this has to be -- it was set
     * before it and silently zeroed, so every file's entry was written
     * back to the root directory at the subdirectory's index. A file
     * created in /etc then overwrote whatever happened to be at that
     * slot in /, which is a spectacular way to lose a program.
     */
    f->dir = open_dir;
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
    struct dir target;
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
    err = dir_write_in(&target, (u32)idx, ent);
    if (err != 0) {
        return err;
    }
    return fat_flush_all();
}

static int fat_rename(const char *from, const char *to)
{
    struct dir fdir, tdir;
    char f83[11], t83[11];
    u8 ent[DIRENT_SIZE];
    int idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, from, &fdir, f83, 0);
    if (err != 0) {
        return err;
    }
    err = path_walk(&cwd, to, &tdir, t83, 0);
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
    err = dir_write_in(&fdir, (u32)idx, ent);
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
    struct dir d;
    char n83[11];
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
    err = path_walk(&cwd, name, &d, n83, &last_is_dir);
    if (err != 0) {
        return err;
    }
    if (last_is_dir) {
        /* A path ending in a slash, or "/" itself: a directory, and
         * there is no entry anywhere describing the root. */
        memset(out, 0, sizeof(*out));
        out->d_mode = S_IFDIR | 0755;
        return 0;
    }

    idx = dir_lookup_in(&d, n83, ent);
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

    for (i = 0; i < dir_entries(&cwd); i++) {
        int err = dir_read_in(&cwd, i, ent);

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
    struct dir parent, self;
    char n83[11];
    u8 ent[DIRENT_SIZE];
    u32 cl;
    u16 date, time;
    int idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, path, &parent, n83, 0);
    if (err != 0) {
        return err;
    }
    if (dir_lookup_in(&parent, n83, 0) >= 0) {
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

    idx = dir_create_in(&parent, n83, ATTR_DIR);
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
    struct dir parent, self;
    char n83[11];
    u8 ent[DIRENT_SIZE];
    u32 i, n;
    int idx, err;

    if (!mounted) {
        return -ENODEV;
    }
    err = path_walk(&cwd, path, &parent, n83, 0);
    if (err != 0) {
        return err;
    }
    idx = dir_lookup_in(&parent, n83, ent);
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
    0
};

int fat16_init(void)
{
    return vfs_register(&fat16_type);
}
