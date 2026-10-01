/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * umount - take a volume out of the tree.
 *
 *   umount DIR...
 *
 * Refused (EBUSY) while anything is using the volume: a file open on
 * it, a program running from it, a working directory in it, or another
 * volume mounted on it. There is no lazy unmount.
 */
#include "ulib.h"

static const char *why(s32 err)
{
    switch (-err) {
    case ENOENT: return "no such directory";
    case EINVAL: return "not a mount point";
    case EBUSY:  return "in use";
    case EPERM:  return "only root can unmount";
    default:     return "failed";
    }
}

int main(int argc, char **argv)
{
    int i, rc = 0;

    if (argc < 2) {
        eputs("usage: umount DIR...\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        s32 r = syscall(__NR_umount2, (u32)argv[i], 0);

        if (r < 0) {
            eputs("umount: ");
            eputs(argv[i]);
            eputs(": ");
            eputs(why(r));
            eputs("\n");
            rc = 32;
        }
    }
    return rc;
}
