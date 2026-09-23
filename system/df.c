/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * df - how much of the disk is gone.
 *
 * There is one filesystem on this machine, so there is no mount table
 * to walk and no argument that selects between volumes; df prints the
 * one volume, and an argument is accepted only to say which units.
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
    eputs("usage: df [-h] [-k] [-i]\n"
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

int main(int argc, char **argv)
{
    struct statfs sf;
    struct fslabel fl;
    int human_readable = 0, inodes = 0;
    int i, n;
    u64 total, avail, used;
    const char *name;

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
        } else {
            eputs("df: ");
            eputs(argv[i]);
            eputs(": no such option\n");
            usage();
            return 1;
        }
    }

    if (syscall(__NR_statfs, (u32)&sf, 0, 0) < 0) {
        eputs("df: no filesystem is mounted\n");
        return 1;
    }
    if (syscall(__NR_fsctl, FSCTL_LABEL, 0, (u32)&fl) < 0) {
        fl.name[0] = '\0';
    }
    name = fl.name[0] ? fl.name : "(none)";

    if (inodes) {
        /*
         * FAT has no inode table: a directory entry IS the inode, and
         * there is no fixed number of them. Saying so is more use than
         * printing zeroes as though they were a measurement.
         */
        puts("filesystem      inodes   used   free  use%\n");
        puts(name);
        for (n = (int)strlen(name); n < 16; n++) {
            putch(' ');
        }
        puts("     -      -      -     -   (fat16 has no inode table)\n");
        return 0;
    }

    total = (u64)sf.f_blocks * sf.f_bsize;
    avail = (u64)sf.f_bavail * sf.f_bsize;
    used = total - (u64)sf.f_bfree * sf.f_bsize;

    if (human_readable) {
        puts("filesystem       size   used  avail  use%  mounted on\n");
    } else {
        puts("filesystem  1K-blocks       used      avail  use%  mounted on\n");
    }
    puts(name);
    for (n = (int)strlen(name); n < 12; n++) {
        putch(' ');
    }
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
    puts("%  /\n");
    return 0;
}
