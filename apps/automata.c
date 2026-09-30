/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * automata.c - more cellular automata, and ants.
 *
 *     sage$ automata -k KIND [options]   (automata -h lists them)
 *
 *   brain      Brian's Brain (Brian Silverman): a cell is off, on or
 *              dying. Off turns on with exactly two neighbours on; on
 *              always starts to die; dying always turns off. Nothing is
 *              ever still, and the board fills with "gliders" that
 *              breed more of themselves.
 *   cyclic     the cyclic automaton (David Griffeath): N states round a
 *              circle; a cell moves on to the next state when at least
 *              T neighbours are already in it. From noise, spirals.
 *   wireworld  Brian Silverman's again, for circuits: conductor, and
 *              electron heads and tails running along it. A head
 *              becomes a tail, a tail conductor, and conductor becomes a
 *              head with one or two heads beside it. Drawn here as
 *              loops that each carry an electron, crossing and joining
 *              at random, so the pulses meet and interfere.
 *   ant        Langton's ant, generalised: the rule is a string of L
 *              and R, one letter per colour. The ant turns as its
 *              square's colour says, steps the square on to the next
 *              colour, and moves. "RL" is Langton's -- chaos for ten
 *              thousand steps, then the highway. -a starts several.
 *   turmite    a Turing machine on the plane: an ant with an internal
 *              state, whose table says for each (state, colour) what to
 *              write, how to turn and which state to go to. This one is
 *              Ed Pegg Jr.'s Fibonacci spiral.
 *
 * Keys: space pauses, n steps while paused, r starts again, q stops.
 */
#include "gfx.h"
#include "malloc.h"

enum { K_BRAIN, K_CYCLIC, K_WIRE, K_ANT, K_TURMITE };

static const char *const kinds[] = {
    "brain", "cyclic", "wireworld", "ant", "turmite"
};
#define NKINDS  (sizeof(kinds) / sizeof(kinds[0]))

enum { P_KIND, P_CELL, P_STATES, P_THRESH, P_RULE, P_ANTS, P_SPEED, P_FPS,
       P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_KIND]   = GFX_OPT_STR('k', "kind", "brain"),
    [P_CELL]   = GFX_OPT_NUM('z', "cell size", 4, 1, 16, "px"),
    [P_STATES] = GFX_OPT_NUM('s', "cyclic: states", 14, 3, 32, ""),
    [P_THRESH] = GFX_OPT_NUM('t', "cyclic: threshold", 1, 1, 8, ""),
    [P_RULE]   = GFX_OPT_STR('r', "ant: rule, one L or R a colour", "RL"),
    [P_ANTS]   = GFX_OPT_NUM('a', "ant, turmite: how many", 1, 1, 16, ""),
    [P_SPEED]  = GFX_OPT_NUM('S', "ant, turmite: steps a frame", 200, 1,
                             100000, ""),
    [P_FPS]    = GFX_OPT_NUM('F', "frames per second", 20, 0, 200,
                             "0 for as fast as it will go"),
};

static void more_help(void)
{
    u32 i;

    puts("kinds:");
    for (i = 0; i < NKINDS; i++) {
        puts(" ");
        puts(kinds[i]);
    }
    puts("\nant rules to try: RL, RLR, LLRR, LRRRRRLLR, RRLLLRLLLRRR\n");
}

static struct gfx g;
static int kind, gw, gh;
static u8 *cur, *nxt;
static u8 rowbuf[1024] __attribute__((aligned(4)));
static u32 generation;

/* Each kind's states, and the palette entry each is drawn in. */
static u8 ink[32];
static u32 nstates;

/* Ants and turmites. */
struct ant {
    int x, y, dir, state;       /* dir: 0 up, 1 right, 2 down, 3 left */
};
static struct ant ants[16];
static u32 nants;
static char rule[32];

/* Pegg's Fibonacci turmite: [state][colour] = {write, turn, next},
 * turn 0 none, 1 right, 2 back, 3 left. */
static const u8 fib[2][2][3] = {
    { { 1, 3, 1 }, { 1, 3, 1 } },
    { { 1, 1, 1 }, { 0, 0, 0 } },
};

#define AT(b, x, y) (b)[(u32)(y) * (u32)gw + (u32)(x)]

static void make_palette(void)
{
    u32 i;

    switch (kind) {
    case K_BRAIN:
        nstates = 3;
        gfx_colour(&g, GFX_PAL_FREE + 0, 0x000000UL);
        gfx_colour(&g, GFX_PAL_FREE + 1, 0xffffffUL);
        gfx_colour(&g, GFX_PAL_FREE + 2, 0x3060ffUL);
        break;
    case K_CYCLIC: {
        static const u32 keys[4] = {
            0xff3030UL, 0xffd030UL, 0x30c0ffUL, 0xa040ffUL
        };

        nstates = par[P_STATES].value;
        gfx_ramp(&g, GFX_PAL_FREE, nstates, keys, 4, 1);
        break;
    }
    case K_WIRE:
        nstates = 4;
        gfx_colour(&g, GFX_PAL_FREE + 0, 0x000000UL);   /* empty      */
        gfx_colour(&g, GFX_PAL_FREE + 1, 0xc08020UL);   /* conductor  */
        gfx_colour(&g, GFX_PAL_FREE + 2, 0x80c0ffUL);   /* head       */
        gfx_colour(&g, GFX_PAL_FREE + 3, 0xff4020UL);   /* tail       */
        break;
    default: {
        static const u32 keys[4] = {
            0x000000UL, 0x40ff80UL, 0xff40a0UL, 0x4080ffUL
        };

        nstates = kind == K_ANT ? (u32)strlen(rule) : 2;
        gfx_ramp(&g, GFX_PAL_FREE, nstates, keys, nstates < 4 ? nstates : 4,
                 0);
        gfx_colour(&g, GFX_PAL_FREE + nstates, 0xff2020UL);   /* the ants */
        break;
    }
    }
    for (i = 0; i < 32; i++) {
        ink[i] = (u8)(GFX_PAL_FREE + i);
    }
}

/* A rectangle of conductor carrying one electron, head and tail. */
static void wire_loop(int x0, int y0, int w, int h)
{
    int x, y;

    for (x = 0; x < w; x++) {
        AT(cur, (x0 + x) % gw, y0 % gh) = 1;
        AT(cur, (x0 + x) % gw, (y0 + h - 1) % gh) = 1;
    }
    for (y = 0; y < h; y++) {
        AT(cur, x0 % gw, (y0 + y) % gh) = 1;
        AT(cur, (x0 + w - 1) % gw, (y0 + y) % gh) = 1;
    }
    AT(cur, (x0 + 1) % gw, y0 % gh) = 2;
    AT(cur, x0 % gw, y0 % gh) = 3;
}

static void seed_board(void)
{
    u32 n = (u32)gw * (u32)gh, i;

    memset(cur, 0, n);
    generation = 0;
    switch (kind) {
    case K_BRAIN:
        for (i = 0; i < n; i++) {
            cur[i] = gfx_rand() % 100 < 20 ? 1 : 0;
        }
        break;
    case K_CYCLIC:
        for (i = 0; i < n; i++) {
            cur[i] = (u8)(gfx_rand() % nstates);
        }
        break;
    case K_WIRE:
        for (i = 0; i < 12 + (u32)(gw * gh) / 1500; i++) {
            wire_loop((int)(gfx_rand() % (u32)gw), (int)(gfx_rand() % (u32)gh),
                      8 + (int)(gfx_rand() % 40), 6 + (int)(gfx_rand() % 30));
        }
        break;
    default:
        nants = par[P_ANTS].value;
        for (i = 0; i < nants; i++) {
            ants[i].x = nants == 1 ? gw / 2 : (int)(gfx_rand() % (u32)gw);
            ants[i].y = nants == 1 ? gh / 2 : (int)(gfx_rand() % (u32)gh);
            ants[i].dir = (int)(gfx_rand() % 4);
            ants[i].state = 0;
        }
        break;
    }
}

static int wrapx(int x) { return x < 0 ? x + gw : x >= gw ? x - gw : x; }
static int wrapy(int y) { return y < 0 ? y + gh : y >= gh ? y - gh : y; }

/* How many of the eight neighbours of (x, y) are in state s. */
static u32 count(int x, int y, u8 s)
{
    int dx, dy;
    u32 n = 0;

    for (dy = -1; dy <= 1; dy++) {
        const u8 *row = cur + (u32)wrapy(y + dy) * (u32)gw;

        for (dx = -1; dx <= 1; dx++) {
            if ((dx || dy) && row[wrapx(x + dx)] == s) {
                n++;
            }
        }
    }
    return n;
}

static void step_ca(void)
{
    int x, y;
    u8 *t;

    for (y = 0; y < gh; y++) {
        for (x = 0; x < gw; x++) {
            u8 c = AT(cur, x, y), n;

            switch (kind) {
            case K_BRAIN:
                n = c == 1 ? 2 : c == 2 ? 0 : (count(x, y, 1) == 2 ? 1 : 0);
                break;
            case K_CYCLIC: {
                u8 up = (u8)((c + 1) % nstates);

                n = count(x, y, up) >= par[P_THRESH].value ? up : c;
                break;
            }
            default: {                  /* wireworld */
                u32 heads;

                if (c == 2) {
                    n = 3;
                } else if (c == 3) {
                    n = 1;
                } else if (c == 1) {
                    heads = count(x, y, 2);
                    n = heads == 1 || heads == 2 ? 2 : 1;
                } else {
                    n = 0;
                }
                break;
            }
            }
            AT(nxt, x, y) = n;
        }
    }
    t = cur; cur = nxt; nxt = t;
    generation++;
}

static void step_ants(void)
{
    u32 s, i;

    for (s = 0; s < par[P_SPEED].value; s++) {
        for (i = 0; i < nants; i++) {
            struct ant *a = &ants[i];
            u8 *sq = &AT(cur, a->x, a->y);

            if (kind == K_ANT) {
                a->dir = (a->dir + (rule[*sq] == 'R' ? 1 : 3)) & 3;
                *sq = (u8)((*sq + 1) % nstates);
            } else {
                const u8 *t = fib[a->state][*sq];

                *sq = t[0];
                a->dir = (a->dir + t[1]) & 3;
                a->state = t[2];
            }
            switch (a->dir) {
            case 0: a->y = wrapy(a->y - 1); break;
            case 1: a->x = wrapx(a->x + 1); break;
            case 2: a->y = wrapy(a->y + 1); break;
            default: a->x = wrapx(a->x - 1); break;
            }
        }
        generation++;
    }
}

static void render(void)
{
    u32 cell = par[P_CELL].value, x, y, k, i;
    u8 *draw = gfx_frame(&g);

    for (y = 0; y < (u32)gh; y++) {
        const u8 *row = cur + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = ink[row[x] & 31];

            for (k = 0; k < cell; k++) {
                *p++ = c;
            }
        }
        if (kind == K_ANT || kind == K_TURMITE) {
            for (i = 0; i < nants; i++) {
                if (ants[i].y == (int)y) {
                    for (k = 0; k < cell; k++) {
                        rowbuf[(u32)ants[i].x * cell + k] =
                            (u8)(GFX_PAL_FREE + nstates);
                    }
                }
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
    memset(rowbuf, GFX_PAL_FREE, g.info.width);
    for (y = (u32)gh * cell; y < g.info.height; y++) {
        gfx_copy_row(draw + y * g.info.pitch, rowbuf, g.info.width);
    }
    gfx_flip(&g);
}

static void step(void)
{
    if (kind == K_ANT || kind == K_TURMITE) {
        step_ants();
    } else {
        step_ca();
    }
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    u32 i, seed;
    int k, r;

    r = gfx_options(argc, argv, "automata", par, P_NPARAM, more_help);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    for (i = 0; i < NKINDS && strcmp(par[P_KIND].str, kinds[i]); i++) {
    }
    if (i == NKINDS) {
        eputs("automata: no kind called ");
        eputs(par[P_KIND].str);
        eputs(" (automata -h lists them)\n");
        return 2;
    }
    kind = (int)i;
    if (kind == K_ANT) {
        const char *s = par[P_RULE].str;
        u32 n = (u32)strlen(s);

        if (n < 2 || n > 16) {
            eputs("automata: an ant rule is 2 to 16 letters\n");
            return 2;
        }
        for (i = 0; i < n; i++) {
            if (s[i] != 'L' && s[i] != 'R') {
                eputs("automata: an ant rule is only L and R\n");
                return 2;
            }
        }
        memcpy(rule, s, n + 1);
    }
    if (gfx_open(&g, "automata", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf)) {
        gfx_close(&g);
        eputs("automata: this screen is wider than it can draw\n");
        return 1;
    }
    if (kind == K_ANT || kind == K_TURMITE) {
        if (par[P_CELL].value == 4) {
            par[P_CELL].value = 2;          /* ants want room */
        }
    }
    gw = (int)(g.info.width / par[P_CELL].value);
    gh = (int)(g.info.height / par[P_CELL].value);
    cur = calloc((u32)gw * (u32)gh, 1);
    nxt = calloc((u32)gw * (u32)gh, 1);
    if (!cur || !nxt) {
        gfx_close(&g);
        eputs("automata: not enough memory\n");
        return 1;
    }
    seed = gfx_seed();
    make_palette();
    seed_board();

    puts("automata: ");
    puts(kinds[kind]);
    puts(", ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(", seed ");
    putdec(seed);
    puts("\nspace pauses, n steps, r starts again, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        if (k == 'r') {
            seed_board();
        } else if (k == ' ') {
            u32 at = gfx_clock_pause(&clk);

            while (!gfx_quit_key(k = gfx_key_wait()) && k != ' ') {
                if (k == 'n') {
                    step();
                    render();
                }
            }
            gfx_clock_resume(&clk, at);
            if (gfx_quit_key(k)) {
                break;
            }
        }
        render();
        step();
        gfx_clock_tick(&clk);
    }
    gfx_close(&g);
    puts("automata: ");
    putdec(generation);
    puts(kind == K_ANT || kind == K_TURMITE ? " steps; " : " generations; ");
    gfx_clock_summary(&clk, "automata", "frames");
    putch('\n');
    return 0;
}
