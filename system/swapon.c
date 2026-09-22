/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * swapon - start using a file as swap.
 *
 *   swapon FILE
 *
 * The file must already exist, at its full size -- a swap file is not
 * grown, because the kernel reaches its pages on the disk directly and
 * learns where they are once, here. From the host:
 *
 *   dd if=/dev/zero of=swap.img bs=1M count=16
 *   mcopy -i hd.img@@1048576 swap.img ::/swap
 *
 * or in the machine, `truncate`-style, with any program that extends a
 * file with zeroes. While it is in use the file cannot be written,
 * truncated, renamed or deleted (ETXTBSY).
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
        puts("usage: swapon FILE\n");
        return 2;
    }
    r = syscall(__NR_swapon, (u32)argv[1], 0);
    if (r < 0) {
        puts("swapon: ");
        puts(argv[1]);
        puts(": ");
        puts(why(r));
        putch('\n');
        return 1;
    }
    return 0;
}
