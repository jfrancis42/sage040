/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * plasma.c - plasma and a tunnel, animated by palette cycling.
 *
 *     sage$ plasma [options]     (plasma -h lists them)
 *
 * The oldest trick in the demo scene: draw the picture ONCE, as colour
 * indices, and then never touch a pixel again. Every frame rotates the
 * palette instead -- index i shows the colour that index i+1 showed a
 * frame ago -- so every pixel's colour moves at once, for the price of
 * 224 palette writes and no memory traffic at all. It is why a 1990 PC
 * could show a full screen moving at 70 frames a second.
 *
 *   plasma   a sum of four sine waves of x, y, x+y and the distance from
 *            a point, so the bands flow in several directions at once
 *   tunnel   each pixel's index is its depth -- a constant over its
 *            distance from the centre -- with alternate sectors round
 *            the wall half a cycle out of step: a checkered tube, whose
 *            rings the rotating palette carries into the distance
 *
 * Keys: space pauses, k switches between the two, r picks new colours,
 * + and - change the speed, q stops.
 */
#include "gfx.h"
#include "malloc.h"
#include <math.h>

#define FIRST   GFX_PAL_FREE            /* the cycling part of the palette */
#define NCOL    (256 - GFX_PAL_FREE)    /* 224 entries                     */

enum { P_KIND, P_SPEED, P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_KIND]  = GFX_OPT_STR('k', "kind: plasma or tunnel", "plasma"),
    [P_SPEED] = GFX_OPT_NUM('s', "palette steps a frame", 2, 1, 32, ""),
    [P_FPS]   = GFX_OPT_NUM('F', "frames per second", 30, 1, 200, ""),
};

static struct gfx g;
static int tunnel;
static u32 colours[NCOL];

/* Four to six random key colours, blended round into a cyclic ramp. */
static void new_colours(void)
{
    u32 keys[6], n = 4 + gfx_rand() % 3, i;

    for (i = 0; i < n; i++) {
        keys[i] = gfx_rand() & 0xffffffUL;
    }
    keys[0] = 0x000000UL;               /* one dark band, for contrast */
    for (i = 0; i < NCOL; i++) {
        u32 pos = i * n, k = pos / NCOL, frac = pos % NCOL;

        colours[i] = gfx_blend(keys[k], keys[(k + 1) % n], frac, NCOL);
    }
}

static double atan2d(double y, double x)
{
    if (x > 0) {
        return atan(y / x);
    }
    if (x < 0) {
        return atan(y / x) + (y >= 0 ? M_PI : -M_PI);
    }
    return y > 0 ? M_PI_2 : y < 0 ? -M_PI_2 : 0;
}

/* The index of every pixel, into a row at a time and on to the screen. */
static void draw_index(u8 *draw)
{
    int w = (int)g.info.width, h = (int)g.info.height, x, y;
    int cx = w / 2, cy = h / 2;

    for (y = 0; y < h; y++) {
        u8 *row = draw + (u32)y * g.info.pitch;

        for (x = 0; x < w; x++) {
            int v;

            if (!tunnel) {
                int dx = x - w * 3 / 4, dy = y - h / 3;
                u32 d = gfx_isqrt((u32)(dx * dx + dy * dy));

                /* Four waves, each Q12, summed and scaled to 0..NCOL-1. */
                v = gfx_sin(x * 2) + gfx_sin(y * 3) +
                    gfx_sin((x + y) * 3 / 2) + gfx_sin((int)d * 4);
                v = (v + 4 * GFX_ONE) * (NCOL - 1) / (8 * GFX_ONE);
            } else {
                int dx = x - cx, dy = y - cy;
                double dist = sqrt((double)(dx * dx + dy * dy)) + 1.0;
                double ang = atan2d((double)dy, (double)dx);
                int depth = (int)(40000.0 / dist);
                int sector = (int)((ang + M_PI) * 16 / (2 * M_PI));

                /* Rings by depth, and every other sector of sixteen
                 * round the wall half a cycle out of step: a
                 * checkerboard on the inside of a tube, which the
                 * rotating palette carries away from the viewer. */
                v = (depth + (sector & 1) * (NCOL / 2)) % NCOL;
            }
            row[x] = (u8)(FIRST + v);
        }
    }
}

/* The same picture in both buffers, so a flip never shows the other. */
static void draw_picture(void)
{
    draw_index(gfx_frame(&g));
    gfx_flip(&g);
    draw_index(gfx_frame(&g));
    gfx_flip(&g);
}

static void set_palette(u32 offset)
{
    u32 i;

    for (i = 0; i < NCOL; i++) {
        gfx_colour(&g, FIRST + i, colours[(i + offset) % NCOL]);
    }
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    u32 offset = 0, seed;
    int k, r;

    r = gfx_options(argc, argv, "plasma", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (strcmp(par[P_KIND].str, "tunnel") == 0) {
        tunnel = 1;
    } else if (strcmp(par[P_KIND].str, "plasma") != 0) {
        eputs("plasma: the kinds are plasma and tunnel\n");
        return 2;
    }
    if (gfx_open(&g, "plasma", GFX_MAP) < 0) {
        return 1;
    }
    seed = gfx_seed();
    new_colours();
    set_palette(0);
    draw_picture();

    puts("plasma: ");
    puts(tunnel ? "tunnel" : "plasma");
    puts(", seed ");
    putdec(seed);
    puts("\nspace pauses, k switches, r new colours, + - speed, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case 'k':
            tunnel = !tunnel;
            draw_picture();
            break;
        case 'r':
            new_colours();
            break;
        case '+':
        case '=':
            if (par[P_SPEED].value < 32) par[P_SPEED].value++;
            break;
        case '-':
            if (par[P_SPEED].value > 1) par[P_SPEED].value--;
            break;
        case ' ': {
            u32 at = gfx_clock_pause(&clk);

            while (!gfx_quit_key(k = gfx_key_wait()) && k != ' ') {
            }
            gfx_clock_resume(&clk, at);
            if (gfx_quit_key(k)) {
                goto done;
            }
            break;
        }
        }
        offset = (offset + par[P_SPEED].value) % NCOL;
        set_palette(offset);
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);
    gfx_clock_summary(&clk, "plasma", "frames");
    putch('\n');
    return 0;
}
