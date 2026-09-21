/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fbtest.c - draw one of everything on /dev/fb0 and hold it there.
 *
 * Exists to tell a broken framebuffer driver apart from a broken program
 * that uses one. It draws a filled rectangle, an outline, two diagonals
 * and a row of single points, in different colours, then waits so the
 * screen can be looked at.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    struct fb_info info;
    struct fb_rect r;
    struct fb_line l;
    struct fb_point p;
    int fb, i;
    u32 hold = 5;

    if (argc > 1) {
        hold = (u32)(argv[1][0] - '0');
    }

    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        eputs("fbtest: no /dev/fb0\n");
        return 1;
    }
    if (ioctl(fb, FBIO_GETINFO, (u32)&info) < 0) {
        eputs("fbtest: FBIO_GETINFO failed\n");
        return 1;
    }

    puts("fbtest: ");
    putdec(info.width);
    putch('x');
    putdec(info.height);
    putch('x');
    putdec(info.bpp);
    puts(" pitch ");
    putdec(info.pitch);
    putch('\n');

    if (ioctl(fb, FBIO_CLEAR, 0) < 0) {
        eputs("fbtest: FBIO_CLEAR failed\n");
    }

    /* A filled rectangle, through the blitter. */
    r.x = 40; r.y = 40; r.w = 200; r.h = 120;
    r.colour = 2; r.filled = 1;
    if (ioctl(fb, FBIO_RECT, (u32)&r) < 0) {
        eputs("fbtest: filled FBIO_RECT failed\n");
    }

    /* An outline, which is four lines. */
    r.x = 300; r.y = 40; r.w = 200; r.h = 120;
    r.colour = 3; r.filled = 0;
    if (ioctl(fb, FBIO_RECT, (u32)&r) < 0) {
        eputs("fbtest: outline FBIO_RECT failed\n");
    }

    /* Two diagonals across the whole screen. */
    l.x0 = 0; l.y0 = 0;
    l.x1 = (s32)info.width - 1; l.y1 = (s32)info.height - 1;
    l.colour = 1;
    if (ioctl(fb, FBIO_LINE, (u32)&l) < 0) {
        eputs("fbtest: FBIO_LINE failed\n");
    }
    l.x0 = (s32)info.width - 1; l.y0 = 0;
    l.x1 = 0; l.y1 = (s32)info.height - 1;
    l.colour = 5;
    ioctl(fb, FBIO_LINE, (u32)&l);

    /* A row of points. */
    for (i = 0; i < 100; i++) {
        p.x = 100 + i * 4;
        p.y = 300;
        p.colour = 6;
        ioctl(fb, FBIO_POINT, (u32)&p);
    }

    if (ioctl(fb, FBIO_FLIP, 0) < 0) {
        eputs("fbtest: FBIO_FLIP failed\n");
    }

    puts("fbtest: drawn, holding\n");
    msleep(hold * 1000);
    close(fb);
    return 0;
}
