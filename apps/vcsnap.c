/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * vcsnap MS FILE - after MS milliseconds, copy /dev/vcsa to FILE.
 *
 * Run in the background before a full-screen program, it records what
 * that program put on the screen -- characters, attributes and cursor,
 * in /dev/vcsa's format -- somewhere the host can read it afterwards.
 * It is how a test sees what an editor drew without a camera.
 */
#include "ulib.h"

static u8 buf[4 + 128 * 64 * 2];

int main(int argc, char **argv)
{
    u32 ms = 0;
    const char *p;
    int in, out;
    s32 n;

    if (argc != 3) {
        eputs("usage: vcsnap MS FILE\n");
        return 2;
    }
    for (p = argv[1]; *p >= '0' && *p <= '9'; p++) {
        ms = ms * 10 + (u32)(*p - '0');
    }
    msleep(ms);
    in = open("/dev/vcsa", O_RDONLY);
    if (in < 0) {
        eputs("vcsnap: no /dev/vcsa\n");
        return 1;
    }
    n = read(in, buf, sizeof(buf));
    close(in);
    out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0 || n <= 0 || write(out, buf, (u32)n) != n) {
        eputs("vcsnap: could not write the snapshot\n");
        return 1;
    }
    close(out);
    return 0;
}
