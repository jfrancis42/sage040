/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sandpile.c - the abelian sandpile (Bak, Tang and Wiesenfeld, 1987).
 *
 *     sage$ sandpile [options]   (sandpile -h lists them)
 *
 * Grains are dropped one at a time on the middle of a grid. A square
 * holding four or more TOPPLES: it loses four and gives one to each of
 * its four neighbours, which may topple in turn; grains pushed over the
 * edge are lost. "Abelian" because the order of the topplings does not
 * matter -- the pile settles to the same thing however it is done --
 * which is what lets this keep a plain stack of squares to topple
 * rather than sweeping the whole grid again and again.
 *
 * Every square ends with 0 to 3 grains, drawn in four colours, and the
 * pattern that grows is a fractal no one designed: straight-edged
 * patches, all of them made of smaller ones.
 *
 * Keys: space pauses, + and - drop more or fewer grains a frame, r
 * starts again, q stops.
 */
#include "gfx.h"
#include "malloc.h"

enum { P_CELL, P_GRAINS, P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_CELL]   = GFX_OPT_NUM('z', "cell size", 2, 1, 16, "px"),
    [P_GRAINS] = GFX_OPT_NUM('g', "grains a frame", 256, 1, 100000, ""),
    [P_FPS]    = GFX_OPT_NUM('F', "frames per second", 30, 0, 200,
                             "0 for as fast as it will go"),
};

static struct gfx g;
static int gw, gh;
static u8 *pile;
static u32 *todo;               /* squares that have reached four      */
static u32 ntodo;
static u32 dropped, topples;
static u8 rowbuf[1024] __attribute__((aligned(4)));

static const u32 colours[4] = {
    0x000010UL, 0x2060e0UL, 0xf0c020UL, 0xe03040UL
};

static void add(int x, int y)
{
    u32 i;

    if (x < 0 || y < 0 || x >= gw || y >= gh) {
        return;                         /* off the edge: gone */
    }
    i = (u32)y * (u32)gw + (u32)x;
    if (++pile[i] == 4) {
        todo[ntodo++] = i;
    }
}

static void settle(void)
{
    while (ntodo) {
        u32 i = todo[--ntodo];
        int x = (int)(i % (u32)gw), y = (int)(i / (u32)gw);

        while (pile[i] >= 4) {
            pile[i] -= 4;
            topples++;
            add(x - 1, y);
            add(x + 1, y);
            add(x, y - 1);
            add(x, y + 1);
        }
    }
}

static void reset(void)
{
    memset(pile, 0, (u32)gw * (u32)gh);
    ntodo = 0;
    dropped = topples = 0;
}

static void render(void)
{
    u32 cell = par[P_CELL].value, x, y, k;
    u8 *draw = gfx_frame(&g);

    for (y = 0; y < (u32)gh; y++) {
        const u8 *row = pile + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = (u8)(GFX_PAL_FREE + (row[x] & 3));

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
    u32 i;
    int k, r;

    r = gfx_options(argc, argv, "sandpile", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (gfx_open(&g, "sandpile", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf)) {
        gfx_close(&g);
        eputs("sandpile: this screen is wider than it can draw\n");
        return 1;
    }
    gw = (int)(g.info.width / par[P_CELL].value);
    gh = (int)(g.info.height / par[P_CELL].value);
    pile = malloc((u32)gw * (u32)gh);
    /* Each square is on the stack at most once at a time: it goes on
     * when it reaches exactly four. */
    todo = malloc((u32)gw * (u32)gh * sizeof(u32));
    if (!pile || !todo) {
        gfx_close(&g);
        eputs("sandpile: not enough memory\n");
        return 1;
    }
    for (i = 0; i < 4; i++) {
        gfx_colour(&g, GFX_PAL_FREE + i, colours[i]);
    }
    reset();
    puts("sandpile: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts("\nspace pauses, + - grains a frame, r starts again, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case '+': case '=':
            if (par[P_GRAINS].value < 50000) par[P_GRAINS].value *= 2;
            break;
        case '-':
            if (par[P_GRAINS].value > 1) par[P_GRAINS].value /= 2;
            break;
        case 'r':
            reset();
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
        for (i = 0; i < par[P_GRAINS].value; i++) {
            add(gw / 2, gh / 2);
            settle();
            dropped++;
        }
        render();
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);
    puts("sandpile: ");
    putdec(dropped);
    puts(" grains, ");
    putdec(topples);
    puts(" topples; ");
    gfx_clock_summary(&clk, "sandpile", "frames");
    putch('\n');
    return 0;
}
