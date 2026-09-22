/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * swapoff - stop using the swap file.
 *
 *   swapoff FILE
 *
 * Every page that is out in the file is brought back first, so this
 * fails with ENOMEM if memory cannot hold them all -- and then swap
 * stays on, with none of those pages lost.
 */
#include "ulib.h"

static const char *why(s32 err)
{
    switch (-err) {
    case ENOENT:  return "no such file";
    case EINVAL:  return "not a swap file, or not one that can be used";
    case EBUSY:   return "swap is already on";
    case ENOMEM:  return "not enough memory";
    case ETXTBSY: return "in use";
    default:      return "failed";
    }
}

int main(int argc, char **argv)
{
    s32 r;

    if (argc != 2) {
        puts("usage: swapoff FILE\n");
        return 2;
    }
    r = syscall(__NR_swapoff, (u32)argv[1]);
    if (r < 0) {
        puts("swapoff: ");
        puts(argv[1]);
        puts(": ");
        puts(why(r));
        putch('\n');
        return 1;
    }
    return 0;
}
