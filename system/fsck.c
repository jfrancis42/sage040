/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fsck - check the disk, and with -y, repair it.
 *
 *   fsck        report what is wrong, change nothing
 *   fsck -y     put it right
 *
 * The exit status is fsck's usual one: 0 clean, 1 problems found and
 * all repaired, 4 problems left as they were, 8 the check itself could
 * not be done.
 *
 * The checking is in the filesystem driver (fsctl), which already knows
 * what a chain, a directory and a long name are. A volume that was not
 * cleanly unmounted is checked and repaired at boot; this is for
 * looking on demand, or after writing to the disk from outside.
 */
#include "ulib.h"

static void line(const char *what, u32 n)
{
    if (n) {
        puts("  ");
        putdec(n);
        putch(' ');
        puts(what);
        putch('\n');
    }
}

int main(int argc, char **argv)
{
    struct fsck_report r;
    int repair = argc > 1 && strcmp(argv[1], "-y") == 0;
    s32 err;
    u32 found;

    if (argc > 1 && !repair) {
        eputs("usage: fsck [-y]\n");
        return 8;
    }
    err = syscall(__NR_fsctl, FSCTL_CHECK, repair ? FSCK_REPAIR : 0,
                  (u32)&r);
    if (err < 0) {
        eputs("fsck: ");
        eputs(err == -EBUSY ? "files are open; cannot repair while they are"
                            : "cannot check the volume");
        eputs("\n");
        return 8;
    }

    puts("fsck: ");
    putdec(r.files);
    puts(" files, ");
    putdec(r.dirs + 1);
    puts(" directories, ");
    putdec(r.clusters_used);
    puts(" clusters used, ");
    putdec(r.clusters_free);
    puts(" free\n");

    line("FAT sectors where the two copies disagree", r.fat_mismatch);
    line("chains with a bad link", r.bad_chains);
    line("chains crossing another, or themselves", r.cross_linked);
    line("sizes that did not fit their chain", r.size_fixed);
    line("wrong . or .. entries", r.dot_entries);
    line("long-name entries with no file", r.orphan_lfn);
    line("lost clusters", r.lost_clusters);
    line("directories too deep to check", r.too_deep);

    found = r.fat_mismatch + r.bad_chains + r.cross_linked + r.size_fixed +
            r.dot_entries + r.orphan_lfn + r.lost_clusters;
    if (found == 0) {
        puts("fsck: clean\n");
        return 0;
    }
    if (repair) {
        puts("fsck: repaired\n");
        return 1;
    }
    puts("fsck: not repaired; run fsck -y\n");
    return 4;
}
