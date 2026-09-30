/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * dla.c - diffusion-limited aggregation (Witten and Sander, 1981).
 *
 *     sage$ dla [options]        (dla -h lists them)
 *
 * One particle sits in the middle. Others are let go, one at a time,
 * from a circle round the growing cluster, and wander at random until
 * they touch it -- and stick. Because a wanderer is far likelier to hit
 * a tip that sticks out than to find its way down between two arms,
 * the tips grow fastest, and what grows is the branching coral-and-
 * lightning shape that frost, mineral dendrites and electrical
 * discharges all make for the same reason.
 *
 * Coloured by when each particle stuck, so the rings of growth show. A
 * wanderer that strays too far is started again on the circle; one that
 * reaches the screen's edge ends the cluster, and it starts again.
 *
 * Keys: space pauses, r starts again, q stops.
 */
#include "gfx.h"
#include "malloc.h"

#define FIRST   (GFX_PAL_FREE + 1)
#define NCOL    (256 - FIRST)

enum { P_CELL, P_RATE, P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_CELL] = GFX_OPT_NUM('z', "cell size", 2, 1, 16, "px"),
    [P_RATE] = GFX_OPT_NUM('n', "particles to stick a frame", 20, 1, 10000,
                           ""),
    [P_FPS]  = GFX_OPT_NUM('F', "frames per second", 30, 0, 200,
                           "0 for as fast as it will go"),
};

static struct gfx g;
static int gw, gh, cx, cy;
static u8 *grid;                /* 0 empty, else its colour           */
static u32 stuck, radius, clusters = 1;
static u8 rowbuf[1024] __attribute__((aligned(4)));

static const u32 keys[5] = {
    0xffffffUL, 0x80e0ffUL, 0x3060ffUL, 0xc040ffUL, 0xff4080UL
};

static void reset(void)
{
    memset(grid, 0, (u32)gw * (u32)gh);
    grid[(u32)cy * (u32)gw + (u32)cx] = FIRST;
    stuck = 1;
    radius = 1;
}

static int touching(int x, int y)
{
    return grid[(u32)y * (u32)gw + (u32)(x - 1)] ||
           grid[(u32)y * (u32)gw + (u32)(x + 1)] ||
           grid[(u32)(y - 1) * (u32)gw + (u32)x] ||
           grid[(u32)(y + 1) * (u32)gw + (u32)x];
}

/* Let one go and walk it until it sticks. 0, or -1 if the cluster has
 * reached the edge and it is time for another. */
static int release(void)
{
    u32 launch = radius + 5, kill = radius * 2 + 20;

    for (;;) {
        int a = (int)(gfx_rand() & 255);
        int x = cx + (int)((gfx_cos(a) * (int)launch) >> GFX_FRAC);
        int y = cy + (int)((gfx_sin(a) * (int)launch) >> GFX_FRAC);

        for (;;) {
            int dx, dy;
            u32 d2;

            if (x < 1 || y < 1 || x >= gw - 1 || y >= gh - 1) {
                break;                          /* off the grid: again */
            }
            if (touching(x, y)) {
                u32 d;

                grid[(u32)y * (u32)gw + (u32)x] =
                    (u8)(FIRST + (stuck / 40) % NCOL);
                stuck++;
                dx = x - cx;
                dy = y - cy;
                d = gfx_isqrt((u32)(dx * dx + dy * dy));
                if (d > radius) {
                    radius = d;
                }
                if (x < 3 || y < 3 || x >= gw - 3 || y >= gh - 3) {
                    return -1;
                }
                return 0;
            }
            dx = x - cx;
            dy = y - cy;
            d2 = (u32)(dx * dx + dy * dy);
            if (d2 > kill * kill) {
                break;                          /* strayed: again */
            }
            /* Far out, take big strides: the walk is the same, only
             * sooner over. Near the cluster, one square at a time. */
            {
                int stride = d2 > (radius + 12) * (radius + 12) ?
                             (int)gfx_isqrt(d2) - (int)radius - 8 : 1;
                u32 rr = gfx_rand();

                if (stride < 1) {
                    stride = 1;
                }
                switch (rr & 3) {
                case 0: x += stride; break;
                case 1: x -= stride; break;
                case 2: y += stride; break;
                default: y -= stride; break;
                }
            }
        }
    }
}

static void render(void)
{
    u32 cell = par[P_CELL].value, x, y, k;
    u8 *draw = gfx_frame(&g);

    for (y = 0; y < (u32)gh; y++) {
        const u8 *row = grid + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = row[x] ? row[x] : GFX_PAL_FREE;

            for (k = 0; k < cell; k++) {
                *p++ = c;
            }
        }
        while (p < rowbuf + g.info.width) {
            *p++ = GFX_PAL_FREE;
        }
        for (k = 0; k < cell; k++) {
            gfx_copy_row(draw + (y * cell + k) * g.info.pitch, rowbuf,
                         g.info.width);
        }
    }
    gfx_flip(&g);
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    u32 i, seed;
    int k, r;

    r = gfx_options(argc, argv, "dla", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (gfx_open(&g, "dla", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf)) {
        gfx_close(&g);
        eputs("dla: this screen is wider than it can draw\n");
        return 1;
    }
    gw = (int)(g.info.width / par[P_CELL].value);
    gh = (int)(g.info.height / par[P_CELL].value);
    cx = gw / 2;
    cy = gh / 2;
    grid = malloc((u32)gw * (u32)gh);
    if (!grid) {
        gfx_close(&g);
        eputs("dla: not enough memory\n");
        return 1;
    }
    seed = gfx_seed();
    gfx_colour(&g, GFX_PAL_FREE, 0x000000UL);
    gfx_ramp(&g, FIRST, NCOL, keys, 5, 1);
    reset();
    puts("dla: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(", seed ");
    putdec(seed);
    puts("\nspace pauses, r starts again, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        if (k == 'r') {
            reset();
            clusters++;
        } else if (k == ' ') {
            u32 at = gfx_clock_pause(&clk);

            while (!gfx_quit_key(k = gfx_key_wait()) && k != ' ') {
            }
            gfx_clock_resume(&clk, at);
            if (gfx_quit_key(k)) {
                break;
            }
        }
        for (i = 0; i < par[P_RATE].value; i++) {
            if (release() < 0) {
                render();
                msleep(2000);           /* a moment to look at it */
                reset();
                clusters++;
                break;
            }
        }
        render();
        gfx_clock_tick(&clk);
    }
    gfx_close(&g);
    puts("dla: ");
    putdec(stuck);
    puts(" particles in this cluster, ");
    putdec(clusters);
    puts(" clusters; ");
    gfx_clock_summary(&clk, "dla", "frames");
    putch('\n');
    return 0;
}
