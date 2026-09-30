/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * rd.c - reaction-diffusion: the Gray-Scott model.
 *
 *     sage$ rd [options]         (rd -h lists them)
 *
 * Two chemicals on a grid. U is fed in everywhere at rate F; V is
 * removed at rate F + k; and where they meet, U + 2V makes 3V -- V feeds
 * on U and makes more of itself. Both diffuse, U twice as fast as V.
 * From a few seeded patches, depending on F and k alone, the V grows
 * into spots that divide like cells, into worms and mazes, or into
 * coral -- the kind of pattern Turing suggested in 1952 might be how a
 * leopard gets its spots.
 *
 * Floating point per cell, several times a frame, so the grid is small
 * (-z sets how big a cell is drawn) and the FPU does all of it.
 *
 * Keys: p goes to the next preset, r seeds it again, space pauses, q
 * stops.
 */
#include "gfx.h"
#include "malloc.h"

#define FIRST   GFX_PAL_FREE
#define NCOL    (256 - GFX_PAL_FREE)

enum { P_CELL, P_STEPS, P_FPS, P_PRESET, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_CELL]   = GFX_OPT_NUM('z', "cell size", 4, 2, 16, "px"),
    [P_STEPS]  = GFX_OPT_NUM('n', "steps a frame", 8, 1, 200, ""),
    [P_FPS]    = GFX_OPT_NUM('F', "frames per second", 0, 0, 200,
                             "0 for as fast as it will go"),
    [P_PRESET] = GFX_OPT_STR('p', "preset", "coral"),
};

static const struct {
    const char *name;
    float f, k;
} presets[] = {
    { "coral",   0.0545f, 0.062f },
    { "mitosis", 0.0367f, 0.0649f },
    { "worms",   0.078f,  0.061f },
    { "maze",    0.029f,  0.057f },
    { "spots",   0.035f,  0.065f },
};
#define NPRESETS (sizeof(presets) / sizeof(presets[0]))

static struct gfx g;
static int gw, gh, preset;
static float *u, *v, *u2, *v2;
static u8 rowbuf[1024] __attribute__((aligned(4)));
static u32 steps;

static void more_help(void)
{
    u32 i;

    puts("presets:");
    for (i = 0; i < NPRESETS; i++) {
        puts(" ");
        puts(presets[i].name);
    }
    putch('\n');
}

static void seed_grid(void)
{
    int i, n = gw * gh, s;

    for (i = 0; i < n; i++) {
        u[i] = 1.0f;
        v[i] = 0.0f;
    }
    /*
     * A dozen squares of V, at random. Ten on a side and V at a half:
     * five on a side at a quarter -- the usual recipe -- dies away
     * under every preset here before it can spread, and leaves a blank
     * screen that looks like a broken program.
     */
    for (s = 0; s < 12; s++) {
        int x0 = 2 + (int)(gfx_rand() % (u32)(gw - 14));
        int y0 = 2 + (int)(gfx_rand() % (u32)(gh - 14));
        int x, y;

        for (y = y0; y < y0 + 10; y++) {
            for (x = x0; x < x0 + 10; x++) {
                u[y * gw + x] = 0.25f;
                v[y * gw + x] = 0.5f;
            }
        }
    }
    steps = 0;
}

static void step(void)
{
    float f = presets[preset].f, k = presets[preset].k;
    int x, y;
    float *t;

    for (y = 0; y < gh; y++) {
        int yu = (y ? y - 1 : gh - 1) * gw, yd = (y < gh - 1 ? y + 1 : 0) * gw;
        int yc = y * gw;

        for (x = 0; x < gw; x++) {
            int xl = x ? x - 1 : gw - 1, xr = x < gw - 1 ? x + 1 : 0;
            int i = yc + x;
            float uc = u[i], vc = v[i];
            /* The five-point Laplacian, on a torus. */
            float lu = u[yu + x] + u[yd + x] + u[yc + xl] + u[yc + xr] - 4 * uc;
            float lv = v[yu + x] + v[yd + x] + v[yc + xl] + v[yc + xr] - 4 * vc;
            float uvv = uc * vc * vc;

            u2[i] = uc + 0.2097f * lu - uvv + f * (1.0f - uc);
            v2[i] = vc + 0.105f * lv + uvv - (f + k) * vc;
        }
    }
    t = u; u = u2; u2 = t;
    t = v; v = v2; v2 = t;
    steps++;
}

static void render(void)
{
    u32 cell = par[P_CELL].value, x, y, c;
    u8 *draw = gfx_frame(&g);

    for (y = 0; y < (u32)gh; y++) {
        const float *row = v + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            int i = (int)(row[x] * 3.0f * NCOL);
            u8 col;

            if (i < 0) i = 0;
            if (i >= NCOL) i = NCOL - 1;
            col = (u8)(FIRST + i);
            for (c = 0; c < cell; c++) {
                *p++ = col;
            }
        }
        while (p < rowbuf + g.info.width) {
            *p++ = FIRST;
        }
        for (c = 0; c < cell; c++) {
            gfx_copy_row(draw + (y * cell + c) * g.info.pitch, rowbuf,
                         g.info.width);
        }
    }
    gfx_flip(&g);
}

int main(int argc, char **argv)
{
    static const u32 keys[5] = {
        0x000818UL, 0x103870UL, 0x20a0c0UL, 0xf0f0a0UL, 0xffffffUL
    };
    struct gfx_clock clk;
    u32 i, n;
    int k, r;

    r = gfx_options(argc, argv, "rd", par, P_NPARAM, more_help);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    for (i = 0; i < NPRESETS && strcmp(par[P_PRESET].str, presets[i].name);
         i++) {
    }
    if (i == NPRESETS) {
        eputs("rd: no preset called ");
        eputs(par[P_PRESET].str);
        eputs(" (rd -h lists them)\n");
        return 2;
    }
    preset = (int)i;
    if (gfx_open(&g, "rd", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf)) {
        gfx_close(&g);
        eputs("rd: this screen is wider than it can draw\n");
        return 1;
    }
    gw = (int)(g.info.width / par[P_CELL].value);
    gh = (int)(g.info.height / par[P_CELL].value);
    n = (u32)gw * (u32)gh;
    u = malloc(n * sizeof(float));
    v = malloc(n * sizeof(float));
    u2 = malloc(n * sizeof(float));
    v2 = malloc(n * sizeof(float));
    if (!u || !v || !u2 || !v2) {
        gfx_close(&g);
        eputs("rd: not enough memory\n");
        return 1;
    }
    gfx_seed();
    gfx_ramp(&g, FIRST, NCOL, keys, 5, 0);
    seed_grid();
    puts("rd: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(", ");
    puts(presets[preset].name);
    puts("\np next preset, r seeds again, space pauses, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case 'p':
            preset = (preset + 1) % (int)NPRESETS;
            puts("rd: ");
            puts(presets[preset].name);
            putch('\n');
            seed_grid();
            break;
        case 'r':
            seed_grid();
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
        for (i = 0; i < par[P_STEPS].value; i++) {
            step();
        }
        render();
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);
    puts("rd: ");
    putdec(steps);
    puts(" steps; ");
    gfx_clock_summary(&clk, "rd", "frames");
    putch('\n');
    return 0;
}
