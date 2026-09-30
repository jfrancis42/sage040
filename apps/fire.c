/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fire.c - the demo-scene fire.
 *
 *     sage$ fire [options]       (fire -h lists them)
 *
 * A grid of heat. The bottom row is set at random every frame, hot or
 * cold; every other cell becomes the average of the cells below it,
 * less a little for cooling, so heat rises and fades as it goes. That
 * is all of it: integer adds and a shift, and the palette -- black
 * through red, orange and yellow to white -- does the rest.
 *
 * Worked out on a grid a quarter the screen's size each way (-z) and
 * drawn with each cell as a block: a fire at full resolution would ask
 * a 25 MHz 68040 for 300,000 averages a frame.
 *
 * Keys: space pauses, + and - cool it less or more, w blows it sideways
 * (again stops the wind), q stops.
 */
#include "gfx.h"
#include "malloc.h"

#define FIRST   GFX_PAL_FREE
#define NCOL    (256 - GFX_PAL_FREE)    /* heat 0..223 */

enum { P_CELL, P_COOL, P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_CELL] = GFX_OPT_NUM('z', "cell size", 4, 1, 16, "px"),
    [P_COOL] = GFX_OPT_NUM('c', "cooling a step", 2, 0, 40, ""),
    [P_FPS]  = GFX_OPT_NUM('F', "frames per second", 30, 0, 200,
                           "0 for as fast as it will go"),
};

static struct gfx g;
static int gw, gh;
static u8 *heat;
static u8 rowbuf[1024] __attribute__((aligned(4)));
static int wind;

static const u32 fire_keys[6] = {
    0x000000UL, 0x600000UL, 0xd01000UL, 0xff7000UL, 0xffe040UL, 0xffffffUL
};

static void step(void)
{
    int x, y;
    u32 cool = par[P_COOL].value;
    u8 *bottom = heat + (u32)(gh - 1) * (u32)gw;

    /* The fuel: each cell of the bottom row hot or cold, in runs, so it
     * burns in tongues rather than as noise. */
    for (x = 0; x < gw; x++) {
        if (gfx_rand() % 4 == 0) {
            bottom[x] = (u8)(gfx_rand() & 1 ? NCOL - 1 : 0);
        }
    }
    for (y = 0; y < gh - 1; y++) {
        u8 *row = heat + (u32)y * (u32)gw;
        const u8 *below = row + gw;
        const u8 *below2 = y + 2 < gh ? below + gw : below;

        for (x = 0; x < gw; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x < gw - 1 ? x + 1 : x;
            int xs = x + wind;
            u32 v;

            if (xs < 0) xs = 0;
            if (xs >= gw) xs = gw - 1;
            v = ((u32)below[xl] + below[xs] + below[xr] + below2[xs]) >> 2;
            row[x] = (u8)(v > cool ? v - cool : 0);
        }
    }
}

static void render(void)
{
    u32 cell = par[P_CELL].value, x, y, k;
    u8 *draw = gfx_frame(&g);

    for (y = 0; y < (u32)gh; y++) {
        const u8 *row = heat + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = (u8)(FIRST + row[x]);

            for (k = 0; k < cell; k++) {
                *p++ = c;
            }
        }
        while (p < rowbuf + g.info.width) {
            *p++ = FIRST;
        }
        for (k = 0; k < cell && y * cell + k < g.info.height; k++) {
            gfx_copy_row(draw + (y * cell + k) * g.info.pitch, rowbuf,
                         g.info.width);
        }
    }
    gfx_flip(&g);
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    int k, r;

    r = gfx_options(argc, argv, "fire", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (gfx_open(&g, "fire", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf)) {
        gfx_close(&g);
        eputs("fire: this screen is wider than it can draw\n");
        return 1;
    }
    gw = (int)(g.info.width / par[P_CELL].value);
    gh = (int)((g.info.height + par[P_CELL].value - 1) / par[P_CELL].value);
    heat = calloc((u32)gw * (u32)gh, 1);
    if (!heat) {
        gfx_close(&g);
        eputs("fire: not enough memory\n");
        return 1;
    }
    gfx_seed();
    gfx_ramp(&g, FIRST, NCOL, fire_keys, 6, 0);
    gfx_clear(&g, FIRST);

    puts("fire: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(" cells\nspace pauses, + - cooling, w wind, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case '+':
        case '=':
            if (par[P_COOL].value > 0) par[P_COOL].value--;
            break;
        case '-':
            if (par[P_COOL].value < 40) par[P_COOL].value++;
            break;
        case 'w':
            wind = wind ? 0 : (gfx_rand() & 1 ? 1 : -1);
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
        step();
        render();
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);
    gfx_clock_summary(&clk, "fire", "frames");
    putch('\n');
    return 0;
}
