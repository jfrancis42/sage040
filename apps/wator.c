/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * wator.c - Wa-Tor, A. K. Dewdney's predator and prey planet
 * (Scientific American, "Computer Recreations", December 1984).
 *
 *     sage$ wator [options]      (wator -h lists them, with defaults)
 *
 * A toroidal ocean of fish and sharks. Each chronon every creature,
 * taken in random order, gets one turn:
 *
 *   a fish     moves to a random empty neighbouring cell, if there is
 *              one; once it is old enough it leaves a newborn fish
 *              behind where it was.
 *   a shark    eats a random neighbouring fish if there is one and moves
 *              into its cell; otherwise, if it has gone too long without
 *              eating it starves, and if it has not it moves like a
 *              fish. It breeds the way a fish does.
 *
 * Neighbours are the four cells north, south, east and west. Nothing
 * else is modelled, and the two populations still chase each other
 * round the cycle Lotka and Volterra described: fish boom, sharks boom
 * on the fish, the fish crash, the sharks starve, the fish recover. The
 * strip along the bottom of the screen plots both, each scaled to its
 * own maximum over the strip so that the lag between them shows.
 *
 * Keys: space pauses, r stocks a new ocean, p prints the populations,
 * q stops.
 *
 * Drawn by mapping /dev/fb0 and writing bytes, because an ocean of
 * sixteen thousand cells is sixteen thousand ioctls a frame drawn any
 * other way. The mapping is of video memory itself (see fbmap.c) and
 * takes no RAM.
 */
#include "gfx.h"
#include "malloc.h"

#define WATER       0
#define FISH        1
#define SHARK       2

#define COL_WATER   (GFX_PAL_FREE + 0)
#define COL_FISH    (GFX_PAL_FREE + 1)
#define COL_SHARK   (GFX_PAL_FREE + 2)
#define COL_GRAPH   (GFX_PAL_FREE + 3)
#define COL_RULE    (GFX_PAL_FREE + 4)

static const u32 colours[5] = {
    0x000c24UL,                 /* water: deep navy                  */
    0x40e040UL,                 /* fish                              */
    0xff5030UL,                 /* shark                             */
    0x080808UL,                 /* graph background                  */
    0x405070UL,                 /* the rule above the graph          */
};

/* ---------------------------------------------------------------- */
/* Parameters                                                        */
/* ---------------------------------------------------------------- */

enum {
    P_FISH, P_SHARKS, P_FBREED, P_SBREED, P_STARVE, P_CELL,
    P_GRAPH, P_RESTART, P_PRINT, P_FPS, P_NPARAM
};

/*
 * The defaults keep both species alive through cycle after cycle on
 * the default ocean (160 x 100) -- over 3,400 chronons in testing, with
 * the fish swinging between about 4,800 and 10,600 and the sharks
 * between 900 and 1,900. Dewdney's own 3/10/3 survives too, but its
 * cycles are damped almost flat. Sharks that breed faster, or starve
 * slower, eat the fish out and then die themselves; that is worth
 * seeing too, and -R restarts the ocean when it happens.
 */
static struct gfx_opt par[P_NPARAM] = {
    [P_FISH]    = GFX_OPT_NUM('f', "initial fish", 30, 0, 100,
                              "% of the ocean"),
    [P_SHARKS]  = GFX_OPT_NUM('s', "initial sharks", 5, 0, 100,
                              "% of the ocean"),
    [P_FBREED]  = GFX_OPT_NUM('b', "fish breeding age", 3, 1, 255,
                              "chronons"),
    [P_SBREED]  = GFX_OPT_NUM('B', "shark breeding age", 12, 1, 255,
                              "chronons"),
    [P_STARVE]  = GFX_OPT_NUM('t', "shark starvation time", 4, 1, 255,
                              "chronons"),
    [P_CELL]    = GFX_OPT_NUM('z', "cell size", 4, 1, 32, "px"),
    [P_GRAPH]   = GFX_OPT_NUM('g', "population graph height", 80, 0, 400,
                              "px, 0 for none"),
    [P_RESTART] = GFX_OPT_NUM('R', "restart when a species dies out",
                              1, 0, 1, "0 or 1"),
    [P_PRINT]   = GFX_OPT_NUM('p', "print populations every", 0, 0, 100000,
                              "chronons, 0 for never"),
    [P_FPS]     = GFX_OPT_NUM('F', "chronons per second", 20, 0, 200,
                              "0 for as fast as it will go"),
};


/* ---------------------------------------------------------------- */
/* The ocean                                                         */
/* ---------------------------------------------------------------- */

static int gw, gh;              /* the ocean, in cells               */
static u32 ncells;
static u8 *kind;                /* WATER, FISH or SHARK              */
static u8 *age;                 /* chronons since born or last bred  */
static u8 *hunger;              /* a shark's chronons since a meal   */
static u32 *moved;              /* the chronon it last acted in      */
static u32 *order;              /* this chronon's turn order         */
static u32 chronon;
static u32 nfish, nsharks;

static int alloc_ocean(void)
{
    kind   = malloc(ncells);
    age    = malloc(ncells);
    hunger = malloc(ncells);
    moved  = malloc(ncells * sizeof(u32));
    order  = malloc(ncells * sizeof(u32));
    return kind && age && hunger && moved && order ? 0 : -1;
}

/*
 * Stock the ocean: exactly the asked-for number of each, placed by
 * shuffling, and each with a random age so that the whole first
 * generation does not breed in the same chronon.
 */
static void stock(void)
{
    u32 i, j, t;
    u32 f = ncells * par[P_FISH].value / 100;
    u32 s = ncells * par[P_SHARKS].value / 100;

    for (i = 0; i < ncells; i++) {
        order[i] = i;
    }
    for (i = ncells - 1; i > 0; i--) {
        j = gfx_rand() % (i + 1);
        t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (i = 0; i < ncells; i++) {
        u32 c = order[i];

        kind[c] = i < f ? FISH : i < f + s ? SHARK : WATER;
        age[c] = 0;
        hunger[c] = 0;
        moved[c] = 0;
        if (kind[c] == FISH) {
            age[c] = (u8)(gfx_rand() % par[P_FBREED].value);
        } else if (kind[c] == SHARK) {
            age[c] = (u8)(gfx_rand() % par[P_SBREED].value);
            hunger[c] = (u8)(gfx_rand() % par[P_STARVE].value);
        }
    }
    nfish = f;
    nsharks = s;
    chronon = 0;
}

/* The four neighbours of cell c, north, south, east, west, round the
 * torus. */
static void neighbours(u32 c, u32 n[4])
{
    int x = (int)(c % (u32)gw), y = (int)(c / (u32)gw);
    int xl = x ? x - 1 : gw - 1, xr = x + 1 < gw ? x + 1 : 0;
    int yu = y ? y - 1 : gh - 1, yd = y + 1 < gh ? y + 1 : 0;

    n[0] = (u32)(yu * gw + x);
    n[1] = (u32)(yd * gw + x);
    n[2] = (u32)(y * gw + xr);
    n[3] = (u32)(y * gw + xl);
}

/* A random neighbour of c holding `what`, or -1 if none does. */
static s32 pick(u32 c, u8 what)
{
    u32 n[4], hit[4];
    int i, k = 0;

    neighbours(c, n);
    for (i = 0; i < 4; i++) {
        if (kind[n[i]] == what) {
            hit[k++] = n[i];
        }
    }
    return k ? (s32)hit[gfx_rand() % (u32)k] : -1;
}

/*
 * Move whatever is in `from` to `to`, and if it is old enough to breed,
 * leave a newborn of its kind behind. Both cells are marked as having
 * acted, so neither the mover nor the baby gets a second turn if the
 * shuffled order reaches them later in this chronon.
 */
static void move_to(u32 from, u32 to, u8 breed_age)
{
    u8 k = kind[from];

    kind[to] = k;
    age[to] = age[from];
    hunger[to] = hunger[from];
    moved[to] = chronon;

    if (age[to] >= breed_age) {
        age[to] = 0;
        age[from] = 0;
        hunger[from] = 0;
        moved[from] = chronon;
        if (k == FISH) {
            nfish++;
        } else {
            nsharks++;
        }
    } else {
        kind[from] = WATER;
    }
}

static void fish_turn(u32 c)
{
    s32 to;

    if (age[c] < 255) age[c]++;
    to = pick(c, WATER);
    if (to >= 0) {
        move_to(c, (u32)to, (u8)par[P_FBREED].value);
    }
    /* A fish with nowhere to go neither moves nor breeds -- Dewdney's
     * rule, and what stops a full ocean filling past full. */
}

static void shark_turn(u32 c)
{
    s32 to;

    if (age[c] < 255) age[c]++;
    if (hunger[c] < 255) hunger[c]++;

    to = pick(c, FISH);
    if (to >= 0) {
        nfish--;
        hunger[c] = 0;
        move_to(c, (u32)to, (u8)par[P_SBREED].value);
        return;
    }
    if (hunger[c] >= par[P_STARVE].value) {
        kind[c] = WATER;
        nsharks--;
        return;
    }
    to = pick(c, WATER);
    if (to >= 0) {
        move_to(c, (u32)to, (u8)par[P_SBREED].value);
    }
}

/*
 * One chronon. The order is shuffled every time: sweeping the grid in
 * the same order each chronon would let the creatures early in the
 * sweep always move first, and the whole ocean drifts against the
 * direction of the sweep.
 */
static void step(void)
{
    u32 i, j, t;

    chronon++;
    for (i = ncells - 1; i > 0; i--) {
        j = gfx_rand() % (i + 1);
        t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (i = 0; i < ncells; i++) {
        u32 c = order[i];

        if (moved[c] == chronon) {
            continue;
        }
        if (kind[c] == FISH) {
            fish_turn(c);
        } else if (kind[c] == SHARK) {
            shark_turn(c);
        }
    }
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

static struct gfx g;
static u8 rowbuf[1024] __attribute__((aligned(4)));

/* The graph's history, one sample per pixel column, oldest first
 * starting at hist_at. */
static u32 *hist_fish, *hist_shark;
static u32 hist_at, hist_n;

static void make_palette(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        gfx_colour(&g, (u32)(COL_WATER + i), colours[i]);
    }
}

static void record(void)
{
    u32 w = g.info.width;
    u32 at = (hist_at + hist_n) % w;

    hist_fish[at] = nfish;
    hist_shark[at] = nsharks;
    if (hist_n < w) {
        hist_n++;
    } else {
        hist_at = (hist_at + 1) % w;
    }
}

/* One series, as a line: each column joined to the one before it by a
 * vertical run, so that a steep change still reads as a line and not a
 * scatter of dots. */
static void plot(u8 *top, u32 h, const u32 *hist, u8 colour)
{
    u32 i, max = 1;
    int prev = -1;

    for (i = 0; i < hist_n; i++) {
        u32 v = hist[(hist_at + i) % g.info.width];

        if (v > max) max = v;
    }
    for (i = 0; i < hist_n; i++) {
        u32 v = hist[(hist_at + i) % g.info.width];
        int y = (int)(h - 1) - (int)(v * (h - 1) / max);
        int a = prev < 0 ? y : prev, b = y, t;

        if (a > b) { t = a; a = b; b = t; }
        for (t = a; t <= b; t++) {
            top[(u32)t * g.info.pitch + i] = colour;
        }
        prev = y;
    }
}

static void render(void)
{
    u32 cell = par[P_CELL].value;
    u32 gpx = par[P_GRAPH].value;
    u32 ocean_px = (u32)gh * cell;
    u32 x, y, k;
    u8 *draw;

    /* The buffer to draw in changes with every flip. */
    draw = gfx_frame(&g);

    for (y = 0; y < (u32)gh; y++) {
        const u8 *row = kind + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = (u8)(COL_WATER + row[x]);

            for (k = 0; k < cell; k++) {
                *p++ = c;
            }
        }
        while (p < rowbuf + g.info.width) {
            *p++ = COL_WATER;
        }
        for (k = 0; k < cell; k++) {
            gfx_copy_row(draw + (y * cell + k) * g.info.pitch, rowbuf,
                         g.info.width);
        }
    }
    /* What the cells do not cover, between the ocean and the graph. */
    for (y = ocean_px; y < g.info.height - gpx; y++) {
        gfx_fill_row(draw + y * g.info.pitch, COL_WATER, g.info.width);
    }

    if (gpx) {
        u8 *top = draw + (g.info.height - gpx) * g.info.pitch;

        gfx_fill_row(top, COL_RULE, g.info.width);
        for (y = 1; y < gpx; y++) {
            gfx_fill_row(top + y * g.info.pitch, COL_GRAPH, g.info.width);
        }
        top += g.info.pitch;
        plot(top, gpx - 1, hist_fish, COL_FISH);
        plot(top, gpx - 1, hist_shark, COL_SHARK);
    }

    gfx_flip(&g);
}

static void report(const char *what)
{
    puts("wator: ");
    puts(what);
    puts("chronon ");
    putdec(chronon);
    puts(", ");
    putdec(nfish);
    puts(" fish, ");
    putdec(nsharks);
    puts(" sharks\n");
}

int main(int argc, char **argv)
{
    u32 oceans = 1;
    struct gfx_clock clk;
    u32 seed, every;
    int k, r;

    r = gfx_options(argc, argv, "wator", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (par[P_FISH].value + par[P_SHARKS].value > 100) {
        eputs("wator: more than 100% of the ocean asked for\n");
        return 2;
    }
    every = par[P_PRINT].value;

    if (gfx_open(&g, "wator", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf) ||
        par[P_GRAPH].value + par[P_CELL].value > g.info.height) {
        gfx_close(&g);
        eputs("wator: no room on this screen for that ocean and graph\n");
        return 1;
    }

    gw = (int)(g.info.width / par[P_CELL].value);
    gh = (int)((g.info.height - par[P_GRAPH].value) / par[P_CELL].value);
    ncells = (u32)gw * (u32)gh;
    hist_fish = malloc(g.info.width * sizeof(u32));
    hist_shark = malloc(g.info.width * sizeof(u32));
    if (alloc_ocean() < 0 || !hist_fish || !hist_shark) {
        gfx_close(&g);
        eputs("wator: not enough memory for that ocean\n");
        return 1;
    }

    seed = gfx_seed();
    make_palette();
    stock();

    puts("wator: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(" ocean, ");
    putdec(nfish);
    puts(" fish, ");
    putdec(nsharks);
    puts(" sharks, seed ");
    putdec(seed);
    puts("\nspace pauses, r restocks, p prints the populations, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);

    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case 'r':
            stock();
            hist_at = hist_n = 0;
            oceans++;
            break;
        case 'p':
            report("");
            break;
        case ' ': {
            u32 at = gfx_clock_pause(&clk);

            while (!gfx_quit_key(k = gfx_key_wait()) && k != ' ') {
                if (k == 'p') {
                    report("");
                }
            }
            gfx_clock_resume(&clk, at);
            if (gfx_quit_key(k)) {
                goto done;
            }
            break;
        }
        }

        record();
        render();
        step();

        if (every && chronon % every == 0) {
            report("");
        }
        /* Only a species that was stocked can die out: -s 0 is an
         * ocean of fish on purpose, not a failure to restart from. */
        if (par[P_RESTART].value &&
            ((par[P_SHARKS].value && nsharks == 0) ||
             (par[P_FISH].value && nfish == 0))) {
            report(nsharks == 0 ? "the sharks died out at "
                                : "the fish died out at ");
            stock();
            hist_at = hist_n = 0;
            oceans++;
        }
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);

    report("stopped at ");
    gfx_clock_summary(&clk, "wator", "chronons");
    if (oceans > 1) {
        puts(", ");
        putdec(oceans);
        puts(" oceans");
    }
    putch('\n');
    return 0;
}
