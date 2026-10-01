/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * df - how much of the disk is gone.
 *
 *   df [-h] [-k] [-i] [PATH...]
 *
 * One line for each volume in /proc/mounts, or for the volume each PATH
 * is on. The first column is the volume's label, or its device when it
 * has none; the last is where it is mounted.
 *
 * WHY THIS IS A PROGRAM AND NOT ONLY A SHELL BUILT-IN. The shell has a
 * `df` and keeps it -- a built-in answers when nothing is on the disk
 * to run. But a built-in cannot be piped from a script that is not the
 * shell, cannot be found by `which`, and is not what a program looking
 * for /bin/df will find. Both exist for the same reason /bin/echo does
 * next to the shell's echo.
 *
 * THE ARITHMETIC MULTIPLIES BEFORE IT DIVIDES. Clusters first and
 * kilobytes second throws away everything below one cluster, which on
 * this volume was enough to report a disk with a kernel on it as
 * entirely empty. The intermediate is 64-bit for the same reason: a
 * 512 MB volume in 8 KB clusters is 65536 clusters, and
 * clusters * bytes overflows 32 bits at 4 GB, which is a disk this
 * machine could have.
 */
#include "ulib.h"

static void usage(void)
{
    eputs("usage: df [-h] [-k] [-i] [PATH...]\n"
          "  -h  human-readable sizes (K, M, G)\n"
          "  -k  1024-byte blocks (the default)\n"
          "  -i  inodes -- FAT has none, and df says so\n");
}

/* Right-align a decimal in a field of `w`. */
static void pad_dec(u32 v, int w)
{
    u32 t = v;
    int n = 1;

    while (t >= 10) {
        t /= 10;
        n++;
    }
    while (n++ < w) {
        putch(' ');
    }
    putdec(v);
}

/*
 * A size in bytes as at most four characters and a suffix: 1023, 1.5M,
 * 12G. Rounded to one decimal below 10 of a unit, because "1M" for
 * anything between 1 and 2 megabytes is not worth printing.
 */
static void human(u64 bytes, int w)
{
    static const char suffix[] = " KMGT";
    u64 v = bytes;
    int s = 0;
    u32 whole, frac;
    int n;

    while (v >= 1024 && s < 4) {
        v = v / 1024;
        s++;
    }
    /* One decimal place, from the remainder at the chosen unit. */
    {
        u64 unit = 1;
        int i;

        for (i = 0; i < s; i++) {
            unit *= 1024;
        }
        whole = (u32)(bytes / unit);
        frac = (u32)(((bytes % unit) * 10) / unit);
    }

    /* Width: digits, a possible ".d", and the suffix. */
    n = 1;
    {
        u32 t = whole;

        while (t >= 10) {
            t /= 10;
            n++;
        }
    }
    if (whole < 10 && s > 0) {
        n += 2;                 /* ".d" */
    }
    if (s > 0) {
        n += 1;                 /* the suffix letter */
    }
    while (n++ < w) {
        putch(' ');
    }
    putdec(whole);
    if (whole < 10 && s > 0) {
        putch('.');
        putdec(frac);
    }
    if (s > 0) {
        putch(suffix[s]);
    }
}

#define MAXVOL 16

/* ulib has no strncmp or bounded copy; these are all df needs. */
static int prefix(const char *s, const char *p, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (s[i] != p[i]) {
            return 0;
        }
    }
    return 1;
}

/* Append src to dst, which holds `size` bytes in all. */
static void append(char *dst, const char *src, u32 size)
{
    u32 n = strlen(dst);

    while (*src && n + 1 < size) {
        dst[n++] = *src++;
    }
    dst[n] = '\0';
}

static char mdev[MAXVOL][32], mdir[MAXVOL][256];
static int nmounts;

/* The volumes, from /proc/mounts: the lines whose source is a device. */
static void read_mounts(void)
{
    static char buf[2048];
    int fd = open("/proc/mounts", O_RDONLY);
    s32 n, got = 0;
    char *p;

    if (fd < 0) {
        return;
    }
    while (got < (s32)sizeof(buf) - 1 &&
           (n = read(fd, buf + got, sizeof(buf) - 1 - (u32)got)) > 0) {
        got += n;
    }
    close(fd);
    buf[got] = '\0';
    for (p = buf; *p && nmounts < MAXVOL; ) {
        char *line = p, *sp;
        int k;

        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
        if (!prefix(line, "/dev/", 5)) {
            continue;
        }
        sp = line;
        for (k = 0; *sp && *sp != ' ' && k < 31; k++) {
            mdev[nmounts][k] = *sp++;
        }
        mdev[nmounts][k] = '\0';
        while (*sp == ' ') {
            sp++;
        }
        for (k = 0; *sp && *sp != ' ' && k < 255; k++) {
            mdir[nmounts][k] = *sp++;
        }
        mdir[nmounts][k] = '\0';
        nmounts++;
    }
}

/* The mount point PATH is under: the longest mounted directory that is
 * a prefix of it, as a whole component. */
static int mount_of(const char *path)
{
    static char abs[1024];
    int best = -1, i;
    u32 bl = 0;

    abs[0] = '\0';
    if (path[0] != '/') {
        if (getcwd(abs, sizeof(abs) - 2) < 0) {
            abs[0] = '\0';
        }
        if (strcmp(abs, "/") != 0) {
            append(abs, "/", sizeof(abs));
        }
    }
    append(abs, path, sizeof(abs));
    for (i = 0; i < nmounts; i++) {
        u32 l = (u32)strlen(mdir[i]);

        if ((strcmp(mdir[i], "/") == 0 ||
             (prefix(abs, mdir[i], l) &&
              (abs[l] == '\0' || abs[l] == '/'))) && l >= bl) {
            best = i;
            bl = l;
        }
    }
    return best;
}

static int human_readable, inodes;

static void header(void)
{
    if (inodes) {
        puts("filesystem      inodes       used       free  use%  mounted on\n");
    } else if (human_readable) {
        puts("filesystem       size   used  avail  use%  mounted on\n");
    } else {
        puts("filesystem  1K-blocks       used      avail  use%  mounted on\n");
    }
}

/* One volume's line: the one `path` is on, shown as mounted on `dir`. */
static int row(const char *path, const char *dev, const char *dir)
{
    struct statfs sf;
    struct fslabel fl;
    const char *name;
    u64 total, avail, used;
    int n;

    if (syscall(__NR_statfs, (u32)path, (u32)&sf, 0) < 0) {
        eputs("df: ");
        eputs(path);
        eputs(": cannot read\n");
        return 1;
    }
    if (syscall(__NR_fsctl, FSCTL_LABEL, (u32)path, (u32)&fl) < 0) {
        fl.name[0] = '\0';
    }
    name = fl.name[0] ? fl.name : dev;
    puts(name);
    for (n = (int)strlen(name); n < 12; n++) {
        putch(' ');
    }

    if (inodes) {
        /*
         * A filesystem with a fixed inode table answers with numbers;
         * one without reports zero of them, and saying so is more use
         * than printing zeroes as though they were a measurement. FAT
         * is the second kind -- a directory entry IS the inode there,
         * and there is no fixed number of them -- and ext2 the first.
         */
        u32 ifree = sf.f_ffree;
        u32 iused = sf.f_files - ifree;

        if (sf.f_files == 0) {
            puts("        -          -          -     -"
                 "   (no inode table)\n");
            return 0;
        }
        pad_dec(sf.f_files, 10);
        pad_dec(iused, 11);
        pad_dec(ifree, 11);
        pad_dec((u32)(((u64)iused * 100 + sf.f_files / 2) / sf.f_files), 5);
        puts("%  ");
        puts(dir);
        puts("\n");
        return 0;
    }

    total = (u64)sf.f_blocks * sf.f_bsize;
    avail = (u64)sf.f_bavail * sf.f_bsize;
    used = total - (u64)sf.f_bfree * sf.f_bsize;
    if (human_readable) {
        human(total, 6);
        human(used, 7);
        human(avail, 7);
    } else {
        pad_dec((u32)(total / 1024), 10);
        pad_dec((u32)(used / 1024), 11);
        pad_dec((u32)(avail / 1024), 11);
    }
    pad_dec(total ? (u32)((used * 100 + total / 2) / total) : 0, 5);
    puts("%  ");
    puts(dir);
    puts("\n");
    return 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0, paths = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            human_readable = 1;
        } else if (strcmp(argv[i], "-k") == 0) {
            human_readable = 0;
        } else if (strcmp(argv[i], "-i") == 0) {
            inodes = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            eputs("df: ");
            eputs(argv[i]);
            eputs(": no such option\n");
            usage();
            return 1;
        } else {
            paths++;
        }
    }

    read_mounts();
    if (nmounts == 0) {
        /* No /proc to read: the root, which is always there. */
        append(mdev[0], "/dev/hda1", sizeof(mdev[0]));
        append(mdir[0], "/", sizeof(mdir[0]));
        nmounts = 1;
    }
    header();
    if (!paths) {
        for (i = 0; i < nmounts; i++) {
            rc |= row(mdir[i], mdev[i], mdir[i]);
        }
        return rc;
    }
    for (i = 1; i < argc; i++) {
        int m;

        if (argv[i][0] == '-') {
            continue;
        }
        m = mount_of(argv[i]);
        rc |= row(argv[i], m >= 0 ? mdev[m] : "?", m >= 0 ? mdir[m] : "?");
    }
    return rc;
}
