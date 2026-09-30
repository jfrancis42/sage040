/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * lineprobe.c - lines for kernel/linetest.sh to look at.
 *
 * Every line in every octant, drawn once each way in cells of its own,
 * and lines that run off each edge of the screen beside the same lines
 * moved fully onto it. Nothing here checks anything: the host takes a
 * screendump and kernel/lineprobe.py judges the pixels, so the judge is
 * not the code that drew them.
 */
#include "ulib.h"

#define CELL    80
#define PAD     10

/* The same table is in kernel/lineprobe.py. */
static const s32 specs[][4] = {
    {  0,  0, 60, 20 }, {  0,  0, 20, 60 }, { 60,  0,  0, 20 },
    {  0, 60, 60, 40 }, {  0,  0, 60, 60 }, {  0, 30, 60, 30 },
    { 30,  0, 30, 60 }, {  5,  5,  5,  5 }, {  0,  0, 60, 59 },
    {  0,  0, 59, 60 }, {  0,  0,  3, 60 }, { 10, 10, 47, 13 },
    { 60, 60,  0,  0 }, { 60,  5,  1, 55 }, { 13, 57, 50,  2 },
    {  0,  0,  1,  0 },
};
#define NSPECS  (sizeof(specs) / sizeof(specs[0]))

/* Lines off the edges, and each moved on: {x0, y0, x1, y1, dx, dy}. */
static const s32 clips[][6] = {
    { -30,  20,  50,  60,  200,   0 },      /* off the left   */
    { 320, -20, 360,  30,  200,  40 },      /* off the top    */
    { 600, 450, 700, 500, -300, -40 },      /* off the bottom right */
};
#define NCLIPS  (sizeof(clips) / sizeof(clips[0]))

static int fb;

static int line(s32 x0, s32 y0, s32 x1, s32 y1)
{
    struct fb_line l;

    l.x0 = x0; l.y0 = y0; l.x1 = x1; l.y1 = y1;
    l.colour = 15;
    return ioctl(fb, FBIO_LINE, (u32)&l);
}

int main(void)
{
    u32 i;
    int bad = 0;

    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        eputs("lineprobe: no /dev/fb0\n");
        return 1;
    }
    /* Drawn into the back buffer and flipped, as cube draws: the flip
     * is what tells the display the frame changed. */
    ioctl(fb, FBIO_DOUBLE, 1);
    ioctl(fb, FBIO_CLEAR, 0);
    /* Colour 15 white and 0 black, whatever the console left: an unset
     * palette entry is black, and a black line is no line to the host. */
    {
        struct fb_palette pal;

        pal.index = 15;
        pal.rgb = 0xffffff;
        ioctl(fb, FBIO_PALETTE, (u32)&pal);
        pal.index = 0;
        pal.rgb = 0x000000;
        ioctl(fb, FBIO_PALETTE, (u32)&pal);
    }

    for (i = 0; i < NSPECS; i++) {
        /* Forward in cell 2i, reversed in cell 2i+1, rows 1 to 4. */
        u32 c = 2 * i, d = 2 * i + 1;
        s32 fx = (s32)(c % 8) * CELL + PAD, fy = (s32)(1 + c / 8) * CELL + PAD;
        s32 rx = (s32)(d % 8) * CELL + PAD, ry = (s32)(1 + d / 8) * CELL + PAD;
        const s32 *s = specs[i];

        bad |= line(fx + s[0], fy + s[1], fx + s[2], fy + s[3]) < 0;
        bad |= line(rx + s[2], ry + s[3], rx + s[0], ry + s[1]) < 0;
    }
    for (i = 0; i < NCLIPS; i++) {
        const s32 *c = clips[i];

        bad |= line(c[0], c[1], c[2], c[3]) < 0;
        bad |= line(c[0] + c[4], c[1] + c[5], c[2] + c[4], c[3] + c[5]) < 0;
    }
    /* Out of the engine's range: refused, and nothing drawn. */
    puts(line(-3000, 10, 10, 10) < 0 ? "lineprobe: far line refused\n"
                                     : "lineprobe: far line ACCEPTED\n");
    ioctl(fb, FBIO_SYNC, 0);
    ioctl(fb, FBIO_FLIP, 0);
    puts(bad ? "lineprobe: a line failed\n" : "lineprobe: all drawn\n");
    puts("LP-READY\n");
    msleep(60000);
    close(fb);
    return 0;
}
