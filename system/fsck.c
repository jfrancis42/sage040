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
    putdec(r.blocks_used);
    puts(" blocks used, ");
    putdec(r.blocks_free);
    puts(" free\n");

    line("copies of the metadata that disagree", r.meta_mismatch);
    line("pointers out of range or not free", r.bad_blocks);
    line("blocks shared by two files, or by a loop", r.cross_linked);
    line("sizes that did not fit their blocks", r.size_fixed);
    line("wrong . or .. entries", r.dot_entries);
    line("names with nothing behind them", r.orphan_names);
    line("blocks nothing reaches", r.lost_blocks);
    line("link counts unequal to the names found", r.bad_links);
    line("in-use inodes no name reaches", r.unattached);
    line("free counts unequal to the bitmaps", r.count_mismatch);
    line("directories too deep to check", r.too_deep);

    found = r.meta_mismatch + r.bad_blocks + r.cross_linked + r.size_fixed +
            r.dot_entries + r.orphan_names + r.lost_blocks + r.bad_links +
            r.unattached + r.count_mismatch;
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
