/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * life.c - John Conway's Game of Life (Scientific American, "Mathematical
 * Games", October 1970), and the other "Life-like" rules.
 *
 *     sage$ life [options]       (life -h lists them, with defaults)
 *
 * A toroidal grid of cells, each alive or dead. Every generation, all
 * at once, each cell counts its eight live neighbours: a dead cell with
 * a count in the rule's B set is born, a live one with a count in its S
 * set survives, and everything else is dead. Conway's rule is B3/S23;
 * -r takes any other in the same notation, which is how HighLife
 * (B36/S23, with its replicator), Day & Night (B3678/S34678), Seeds
 * (B2/S) and Maze (B3/S12345) are all one option away.
 *
 * A live cell is coloured by its age -- white when it is born, through
 * yellow and orange to indigo when it has sat still a long time -- so
 * still lifes settle into blue while oscillators and anything moving
 * stay bright. A cell that has died leaves a trail that fades over a
 * few generations, which shows where things have BEEN: a glider draws
 * its own path.
 *
 * When the board has settled -- repeating itself exactly, or its
 * population flat for long enough that nothing is happening but
 * oscillators and gliders -- it starts again, unless -R 0 says not to.
 *
 * Keys: space pauses, n steps a generation while paused, r starts a new
 * board, c switches the colouring, p prints the population, q stops.
 *
 * Drawn the way wator is, by mapping /dev/fb0 and writing bytes; see
 * that program for why.
 */
#include "gfx.h"
#include "malloc.h"

#define COL_MONO    1           /* green, in the driver's palette    */
#define PAL_LIVE    GFX_PAL_FREE /* 16 ages                          */
#define NLIVE       16
#define PAL_TRAIL   (PAL_LIVE + NLIVE) /* 8 fading trail levels      */
#define NTRAIL      8

/*
 * How settled counts as settled. A repeat of the whole board within
 * HASH_RING generations, held for SETTLE_REPEAT generations; or a
 * population that has moved by no more than FLAT_BAND cells over
 * FLAT_GENS generations. The second catches ash with gliders still
 * flying through it, which on a torus can take thousands of generations
 * to repeat exactly.
 */
#define HASH_RING       32
#define SETTLE_REPEAT   60
#define FLAT_GENS       500
#define FLAT_BAND       12

/* ---------------------------------------------------------------- */
/* Patterns                                                          */
/* ---------------------------------------------------------------- */

struct pattern {
    const char *name;
    const char *what;
    const char *const *rows;    /* 'X' alive; 0 = a random soup      */
};

static const char *const p_rpent[] = {
    ".XX",
    "XX.",
    ".X.",
    0
};

static const char *const p_acorn[] = {
    ".X.....",
    "...X...",
    "XX..XXX",
    0
};

static const char *const p_diehard[] = {
    "......X.",
    "XX......",
    ".X...XXX",
    0
};

static const char *const p_gun[] = {
    "........................X...........",
    "......................X.X...........",
    "............XX......XX............XX",
    "...........X...X....XX............XX",
    "XX........X.....X...XX..............",
    "XX........X...X.XX....X.X...........",
    "..........X.....X.......X...........",
    "...........X...X....................",
    "............XX......................",
    0
};

static const char *const p_line[] = {
    "XXXXXXXX.XXXXX...XXX......XXXXXXX.XXXXX",
    0
};

static const struct pattern patterns[] = {
    { "random",  "a random soup at the -d density",               0 },
    { "rpent",   "R-pentomino: 5 cells, 1103 generations of chaos", p_rpent },
    { "acorn",   "acorn: 7 cells, 5206 generations",             p_acorn },
    { "diehard", "diehard: vanishes after 130 generations",      p_diehard },
    { "gun",     "Gosper glider gun, on a torus its own gliders hit", p_gun },
    { "line",    "one row that grows without limit",             p_line },
};
#define NPATTERNS (sizeof(patterns) / sizeof(patterns[0]))

/* ---------------------------------------------------------------- */
/* Parameters                                                        */
/* ---------------------------------------------------------------- */

enum {
    P_CELL, P_DENSITY, P_TRAIL, P_COLOUR, P_RESTART, P_PRINT, P_FPS,
    P_RULE, P_PATTERN, P_NPARAM
};

static struct gfx_opt par[P_NPARAM] = {
    [P_CELL]    = GFX_OPT_NUM('z', "cell size", 4, 1, 32, "px"),
    [P_DENSITY] = GFX_OPT_NUM('d', "random soup density", 25, 1, 100, "%"),
    [P_TRAIL]   = GFX_OPT_NUM('T', "trail length", 8, 0, 255,
                              "generations, 0 for none"),
    [P_COLOUR]  = GFX_OPT_NUM('c', "colour: 0 mono, 1 by age", 1, 0, 1, ""),
    [P_RESTART] = GFX_OPT_NUM('R', "restart when settled", 1, 0, 1,
                              "0 or 1"),
    [P_PRINT]   = GFX_OPT_NUM('v', "print the population every", 0, 0,
                              100000, "generations, 0 for never"),
    [P_FPS]     = GFX_OPT_NUM('F', "generations per second", 20, 0, 200,
                              "0 for as fast as it will go"),
    [P_RULE]    = GFX_OPT_STR('r', "rule, as B<born>/S<survives>", "B3/S23"),
    [P_PATTERN] = GFX_OPT_STR('p', "starting pattern", "random"),
};

static u32 birth, survive;      /* bit n: a count of n qualifies    */
static const struct pattern *pattern = &patterns[0];

/* The end of -h: what the table cannot say. */
static void more_help(void)
{
    u32 i;

    puts("rules to try: B3/S23 is Conway's; B36/S23 HighLife,\n"
         "B3678/S34678 Day & Night, B2/S Seeds, B3/S12345 Maze\n");
    puts("patterns:\n");
    for (i = 0; i < NPATTERNS; i++) {
        puts("  ");
        puts(patterns[i].name);
        puts("  ");
        puts(patterns[i].what);
        putch('\n');
    }
}

static int parse_rule(const char *s)
{
    u32 *set = 0;

    birth = survive = 0;
    for (; *s; s++) {
        if (*s == 'B' || *s == 'b') {
            set = &birth;
        } else if (*s == 'S' || *s == 's') {
            set = &survive;
        } else if (*s == '/') {
            continue;
        } else if (*s >= '0' && *s <= '8' && set) {
            *set |= 1UL << (*s - '0');
        } else {
            return -1;
        }
    }
    /* B0 would have every empty cell born in the first generation --
     * a different kind of automaton, which this does not draw. */
    return set && !(birth & 1) ? 0 : -1;
}

static int check_args(void)
{
    u32 n;

    for (n = 0; n < NPATTERNS; n++) {
        if (strcmp(par[P_PATTERN].str, patterns[n].name) == 0) {
            break;
        }
    }
    if (n == NPATTERNS) {
        eputs("life: no pattern called ");
        eputs(par[P_PATTERN].str);
        eputs("\n");
        return -1;
    }
    pattern = &patterns[n];

    if (parse_rule(par[P_RULE].str) < 0) {
        eputs("life: cannot read the rule ");
        eputs(par[P_RULE].str);
        eputs(" -- it wants the form B3/S23, and no B0\n");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* The board                                                         */
/* ---------------------------------------------------------------- */

/*
 * Two boards with a border of one cell all round, so that a cell's
 * eight neighbours are always at the same eight offsets. The border is
 * a copy of the far edge, remade every generation (wrap_edges), which
 * is what makes the board a torus without a modulo in the inner loop.
 */
static int gw, gh, stride;
static u8 *cur, *nxt;
static u8 *age;                 /* live: generations alive;         */
                                /* dead: generations since it died  */
static u32 generation, population;

static u32 hashes[HASH_RING];
static u32 pops[FLAT_GENS];
static u32 repeats;

static int alloc_board(void)
{
    u32 n = (u32)stride * (u32)(gh + 2);

    cur = calloc(n, 1);
    nxt = calloc(n, 1);
    age = malloc((u32)gw * (u32)gh);
    return cur && nxt && age ? 0 : -1;
}

static void seed_board(void)
{
    u32 i, n = (u32)stride * (u32)(gh + 2);
    int x, y;

    memset(cur, 0, n);
    /* Everything starts as long dead: no trails from before time. */
    memset(age, 255, (u32)gw * (u32)gh);
    population = 0;

    if (!pattern->rows) {
        for (y = 1; y <= gh; y++) {
            for (x = 1; x <= gw; x++) {
                if (gfx_rand() % 100 < par[P_DENSITY].value) {
                    cur[y * stride + x] = 1;
                    age[(y - 1) * gw + (x - 1)] = 0;
                    population++;
                }
            }
        }
    } else {
        int pw = (int)strlen(pattern->rows[0]), ph = 0;

        while (pattern->rows[ph]) {
            ph++;
        }
        for (y = 0; y < ph; y++) {
            for (x = 0; x < pw && pattern->rows[y][x]; x++) {
                /* Centred, and wrapped if the board is smaller. */
                int cx = ((gw - pw) / 2 + x) % gw;
                int cy = ((gh - ph) / 2 + y) % gh;

                if (cx < 0) cx += gw;
                if (cy < 0) cy += gh;
                if (pattern->rows[y][x] == 'X' &&
                    !cur[(cy + 1) * stride + cx + 1]) {
                    cur[(cy + 1) * stride + cx + 1] = 1;
                    age[cy * gw + cx] = 0;
                    population++;
                }
            }
        }
    }

    generation = 0;
    repeats = 0;
    for (i = 0; i < HASH_RING; i++) {
        hashes[i] = 0;
    }
}

static void wrap_edges(u8 *b)
{
    int y;

    memcpy(b, b + gh * stride, (u32)stride);                 /* top    */
    memcpy(b + (gh + 1) * stride, b + stride, (u32)stride);  /* bottom */
    for (y = 0; y < gh + 2; y++) {
        u8 *row = b + y * stride;

        row[0] = row[gw];
        row[gw + 1] = row[1];
    }
}

static void step(void)
{
    int x, y;
    u8 *t;
    u32 pop = 0;

    wrap_edges(cur);
    for (y = 1; y <= gh; y++) {
        const u8 *up = cur + (y - 1) * stride;
        const u8 *mid = cur + y * stride;
        const u8 *dn = cur + (y + 1) * stride;
        u8 *out = nxt + y * stride;
        u8 *ag = age + (y - 1) * gw - 1;

        for (x = 1; x <= gw; x++) {
            u32 n = (u32)(up[x - 1] + up[x] + up[x + 1] +
                          mid[x - 1] +        mid[x + 1] +
                          dn[x - 1] + dn[x] + dn[x + 1]);
            u8 was = mid[x];
            u8 now = (u8)(((was ? survive : birth) >> n) & 1);

            out[x] = now;
            if (now) {
                ag[x] = was ? (ag[x] < 255 ? ag[x] + 1 : 255) : 0;
                pop++;
            } else {
                ag[x] = was ? 0 : (ag[x] < 255 ? ag[x] + 1 : 255);
            }
        }
    }
    t = cur; cur = nxt; nxt = t;
    population = pop;
    generation++;
}

/* FNV-1a over the whole board. A collision could restart a board a
 * little early, which is harmless. */
static u32 board_hash(void)
{
    u32 h = 2166136261UL;
    int x, y;

    for (y = 1; y <= gh; y++) {
        const u8 *row = cur + y * stride;

        for (x = 1; x <= gw; x++) {
            h = (h ^ row[x]) * 16777619UL;
        }
    }
    return h;
}

/* Has nothing been happening for long enough to start again? */
static int settled(void)
{
    u32 h = board_hash();
    u32 i, lo, hi;
    int seen = 0;

    for (i = 0; i < HASH_RING; i++) {
        if (hashes[i] == h) {
            seen = 1;
            break;
        }
    }
    hashes[generation % HASH_RING] = h;
    repeats = seen ? repeats + 1 : 0;
    if (repeats >= SETTLE_REPEAT) {
        return 1;
    }

    pops[generation % FLAT_GENS] = population;
    if (generation < FLAT_GENS) {
        return 0;
    }
    lo = hi = pops[0];
    for (i = 1; i < FLAT_GENS; i++) {
        if (pops[i] < lo) lo = pops[i];
        if (pops[i] > hi) hi = pops[i];
    }
    return hi - lo <= FLAT_BAND;
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

static struct gfx g;
static u8 rowbuf[1024] __attribute__((aligned(4)));
static u8 shade[256];           /* age -> palette entry, dead or alive */
static u8 shade_live[256];

/*
 * Young to old: white, yellow, orange, crimson, indigo. Entries between
 * the key colours are interpolated.
 */
static const u32 live_keys[5] = {
    0xffffffUL, 0xffff40UL, 0xff8030UL, 0xd03070UL, 0x5040c0UL
};

static void make_palette(void)
{
    u32 i, trail = par[P_TRAIL].value;
    int mono = par[P_COLOUR].value == 0;
    u32 ghost = mono ? 0x40ff40UL : 0x2080a0UL;

    gfx_ramp(&g, PAL_LIVE, NLIVE, live_keys, 5, 0);
    /* The trail starts at a third of the ghost colour's brightness and
     * fades to black. */
    for (i = 0; i < NTRAIL; i++) {
        gfx_colour(&g, PAL_TRAIL + i,
                   gfx_blend(0, ghost, NTRAIL - i, NTRAIL * 3));
    }

    for (i = 0; i < 256; i++) {
        shade_live[i] = mono ? COL_MONO
                             : (u8)(PAL_LIVE + (i < NLIVE ? i : NLIVE - 1));
        /* A cell dead for `i` generations: a trail level, or nothing.
         * Age 0 is the generation it died in. */
        shade[i] = i < trail ? (u8)(PAL_TRAIL + i * NTRAIL / trail) : 0;
    }
}

static void render(void)
{
    u32 cell = par[P_CELL].value;
    u32 x, y, k;
    u8 *draw;

    draw = gfx_frame(&g);

    for (y = 0; y < (u32)gh; y++) {
        const u8 *alive = cur + (y + 1) * (u32)stride + 1;
        const u8 *ag = age + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = alive[x] ? shade_live[ag[x]] : shade[ag[x]];

            for (k = 0; k < cell; k++) {
                *p++ = c;
            }
        }
        while (p < rowbuf + g.info.width) {
            *p++ = 0;
        }
        for (k = 0; k < cell; k++) {
            gfx_copy_row(draw + (y * cell + k) * g.info.pitch, rowbuf,
                         g.info.width);
        }
    }
    memset(rowbuf, 0, g.info.width);
    for (y = (u32)gh * cell; y < g.info.height; y++) {
        gfx_copy_row(draw + y * g.info.pitch, rowbuf, g.info.width);
    }

    gfx_flip(&g);
}

static void report(const char *what)
{
    puts("life: ");
    puts(what);
    puts("generation ");
    putdec(generation);
    puts(", population ");
    putdec(population);
    putch('\n');
}

int main(int argc, char **argv)
{
    u32 boards = 1;
    struct gfx_clock clk;
    u32 seed, every;
    int k, r;

    r = gfx_options(argc, argv, "life", par, P_NPARAM, more_help);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (check_args() < 0) {
        eputs("life -h lists the options\n");
        return 2;
    }
    every = par[P_PRINT].value;

    if (gfx_open(&g, "life", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf)) {
        gfx_close(&g);
        eputs("life: this screen is wider than it can draw\n");
        return 1;
    }

    gw = (int)(g.info.width / par[P_CELL].value);
    gh = (int)(g.info.height / par[P_CELL].value);
    stride = gw + 2;
    if (alloc_board() < 0) {
        gfx_close(&g);
        eputs("life: not enough memory for that board\n");
        return 1;
    }

    seed = gfx_seed();
    make_palette();
    seed_board();

    puts("life: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(" board, rule ");
    puts(par[P_RULE].str);
    puts(", ");
    puts(pattern->name);
    puts(", seed ");
    putdec(seed);
    puts("\nspace pauses, n steps, r starts again, c colours, "
         "p prints, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);

    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case 'r':
            seed_board();
            boards++;
            break;
        case 'c':
            par[P_COLOUR].value = !par[P_COLOUR].value;
            make_palette();
            break;
        case 'p':
            report("");
            break;
        case ' ': {
            /* Paused: n steps one generation at a time. */
            u32 at = gfx_clock_pause(&clk);

            while (!gfx_quit_key(k = gfx_key_wait()) && k != ' ') {
                if (k == 'n') {
                    step();
                    render();
                } else if (k == 'p') {
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

        render();
        step();

        if (every && generation % every == 0) {
            report("");
        }
        if (settled() && par[P_RESTART].value) {
            report("settled at ");
            seed_board();
            boards++;
        }
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);

    report("stopped at ");
    gfx_clock_summary(&clk, "life", "generations");
    if (boards > 1) {
        puts(", ");
        putdec(boards);
        puts(" boards");
    }
    putch('\n');
    return 0;
}
