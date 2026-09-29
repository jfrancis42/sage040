/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * attractor.c - strange attractors, drawn as density.
 *
 *     sage$ attractor [options]  (attractor -h lists them, and the kinds)
 *
 * A strange attractor is where a chaotic system spends its time: iterate
 * it long enough from almost anywhere and the points settle onto a
 * shape, never repeating, never leaving. Six are MAPS -- each point is
 * a formula of the last:
 *
 *   clifford   x' = sin(a y) + c cos(a x)      y' = sin(b x) + d cos(b y)
 *   dejong     x' = sin(a y) - cos(b x)        y' = sin(c x) - cos(d y)
 *   svensson   x' = d sin(a x) - sin(b y)      y' = c cos(a x) + cos(b y)
 *   bedhead    x' = sin(x y / b) y + cos(a x - y)   y' = x + sin(y) / b
 *   hopalong   x' = y - sign(x) sqrt|b x - c|   y' = a - x
 *   ikeda      t = 0.4 - 6 / (1 + x^2 + y^2),
 *              x' = 1 + a (x cos t - y sin t)   y' = a (x sin t + y cos t)
 *
 * and two are FLOWS, differential equations stepped with Runge-Kutta:
 *
 *   lorenz     x' = a (y - x)   y' = x (b - z) - y   z' = x y - c z
 *   rossler    x' = -y - z      y' = x + a y         z' = b + z (x - c)
 *
 * Every point adds one to a count for the pixel it lands on, and the
 * screen shows the LOGARITHM of the counts through a smooth palette --
 * a plain count would show the few densest pixels and nothing else. The
 * picture sharpens for as long as it runs: each frame spends most of its
 * time iterating and the rest redrawing from the counts.
 *
 * r looks for a new attractor of the same kind: random parameters, kept
 * only if the orbit stays bounded, is chaotic (a positive Lyapunov
 * exponent -- nearby points separate), and spreads over enough of the
 * screen to be a shape rather than a line or a point. The arrows nudge
 * a and b and redraw, which is how to watch one shape turn into another.
 *
 * The first program here to use the FPU. Its sin, cos, log and sqrt are
 * <math.h>'s, which are the MC68881 instructions themselves (lib/math.h
 * says how the 68040 completes the ones it lacks in silicon).
 */
#include "gfx.h"
#include "malloc.h"
#include <math.h>

#define COL_BG      GFX_PAL_FREE            /* nothing landed here    */
#define COL_FIRST   (GFX_PAL_FREE + 1)      /* one hit                */
#define NLEVELS     (256 - COL_FIRST)       /* 223 levels of density  */

/* ---------------------------------------------------------------- */
/* Maths                                                             */
/* ---------------------------------------------------------------- */

static int finite(double v)
{
    return v == v && v < 1e10 && v > -1e10;
}

/* ---------------------------------------------------------------- */
/* The attractors                                                    */
/* ---------------------------------------------------------------- */

/* s[] is the state -- x, y, z -- and p[] the parameters a, b, c, d. */

static void clifford(double *s, const double *p)
{
    double x = s[0], y = s[1];

    s[0] = sin(p[0] * y) + p[2] * cos(p[0] * x);
    s[1] = sin(p[1] * x) + p[3] * cos(p[1] * y);
}

static void dejong(double *s, const double *p)
{
    double x = s[0], y = s[1];

    s[0] = sin(p[0] * y) - cos(p[1] * x);
    s[1] = sin(p[2] * x) - cos(p[3] * y);
}

static void svensson(double *s, const double *p)
{
    double x = s[0], y = s[1];

    s[0] = p[3] * sin(p[0] * x) - sin(p[1] * y);
    s[1] = p[2] * cos(p[0] * x) + cos(p[1] * y);
}

static void bedhead(double *s, const double *p)
{
    double x = s[0], y = s[1];

    s[0] = sin(x * y / p[1]) * y + cos(p[0] * x - y);
    s[1] = x + sin(y) / p[1];
}

static void hopalong(double *s, const double *p)
{
    double x = s[0], y = s[1];
    double r = sqrt(fabs(p[1] * x - p[2]));

    s[0] = y - (x < 0 ? -r : r);
    s[1] = p[0] - x;
}

static void ikeda(double *s, const double *p)
{
    double x = s[0], y = s[1];
    double t = 0.4 - 6 / (1 + x * x + y * y);
    double ct = cos(t), st = sin(t);

    s[0] = 1 + p[0] * (x * ct - y * st);
    s[1] = p[0] * (x * st + y * ct);
}

/* The flows: a derivative, and one Runge-Kutta step of it with dt = d. */
typedef void (*deriv_fn)(const double *s, const double *p, double *out);

static void lorenz_d(const double *s, const double *p, double *o)
{
    o[0] = p[0] * (s[1] - s[0]);
    o[1] = s[0] * (p[1] - s[2]) - s[1];
    o[2] = s[0] * s[1] - p[2] * s[2];
}

static void rossler_d(const double *s, const double *p, double *o)
{
    o[0] = -s[1] - s[2];
    o[1] = s[0] + p[0] * s[1];
    o[2] = p[1] + s[2] * (s[0] - p[2]);
}

static void rk4(double *s, const double *p, deriv_fn f)
{
    double k1[3], k2[3], k3[3], k4[3], t[3], h = p[3];
    int i;

    f(s, p, k1);
    for (i = 0; i < 3; i++) t[i] = s[i] + h / 2 * k1[i];
    f(t, p, k2);
    for (i = 0; i < 3; i++) t[i] = s[i] + h / 2 * k2[i];
    f(t, p, k3);
    for (i = 0; i < 3; i++) t[i] = s[i] + h * k3[i];
    f(t, p, k4);
    for (i = 0; i < 3; i++) {
        s[i] += h / 6 * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]);
    }
}

static void lorenz(double *s, const double *p)
{
    rk4(s, p, lorenz_d);
}

static void rossler(double *s, const double *p)
{
    rk4(s, p, rossler_d);
}

struct kind {
    const char *name;
    const char *what;
    void (*step)(double *s, const double *p);
    int flow;                   /* a flow, not a map                  */
    int px, py;                 /* which of x, y, z are drawn         */
    int np;                     /* parameters it uses                 */
    double def[4];
    double lo[4], hi[4];        /* where `r` looks                    */
};

static const struct kind kinds[] = {
    { "clifford", "Clifford Pickover's; sines and cosines", clifford,
      0, 0, 1, 4, { -1.4, 1.6, 1.0, 0.7 },
      { -3, -3, -2, -2 }, { 3, 3, 2, 2 } },
    { "dejong", "Peter de Jong's", dejong,
      0, 0, 1, 4, { 1.4, -2.3, 2.4, -2.1 },
      { -3, -3, -3, -3 }, { 3, 3, 3, 3 } },
    { "svensson", "Johnny Svensson's", svensson,
      0, 0, 1, 4, { 1.4, 1.56, 1.4, -6.56 },
      { -3, -3, -3, -7 }, { 3, 3, 3, 7 } },
    { "bedhead", "Ivan Emrich's bedhead; a and b only", bedhead,
      0, 0, 1, 2, { 0.65343, 0.7345345, 0, 0 },
      { -1, 0.2, 0, 0 }, { 1, 1, 0, 0 } },
    { "hopalong", "Barry Martin's; a, b and c; spreads without end",
      hopalong, 0, 0, 1, 3, { 0.4, 1.0, 0.0, 0 },
      { -10, -10, -10, 0 }, { 10, 10, 10, 0 } },
    /* 0.9, not the often-quoted 0.918: in this form of the map 0.918
     * settles to a fixed point from every start tried, while 0.9 is
     * chaotic from near the origin -- and not from (1, 1), since two
     * attractors coexist there. */
    { "ikeda", "Ikeda's laser map; a is the gain, below 1", ikeda,
      0, 0, 1, 1, { 0.9, 0, 0, 0 },
      { 0.6, 0, 0, 0 }, { 0.95, 0, 0, 0 } },
    { "lorenz", "Lorenz's weather; a=sigma b=rho c=beta d=step, x-z",
      lorenz, 1, 0, 2, 3, { 10, 28, 8.0 / 3, 0.004 },
      { 5, 20, 1, 0.004 }, { 16, 60, 4, 0.004 } },
    { "rossler", "Rossler's; a, b, c, d=step, x-y", rossler,
      1, 0, 1, 3, { 0.2, 0.2, 5.7, 0.02 },
      { 0.1, 0.1, 4, 0.02 }, { 0.3, 0.3, 10, 0.02 } },
};
#define NKINDS ((int)(sizeof(kinds) / sizeof(kinds[0])))

/* ---------------------------------------------------------------- */
/* Palettes                                                          */
/* ---------------------------------------------------------------- */

/* Dim to bright. The background is black and separate, so that one
 * hit already shows. */
struct palette {
    const char *name;
    u32 keys[6];
    u32 nkeys;
};

static const struct palette palettes[] = {
    { "ember",    { 0x300800, 0x8a1a00, 0xe05000, 0xffa020, 0xffe080,
                    0xffffff }, 6 },
    { "ice",      { 0x001030, 0x0040a0, 0x00a0e0, 0x80e0ff, 0xffffff },
                  5 },
    { "electric", { 0x200030, 0x8000a0, 0xe020c0, 0xff80e0, 0xffffff },
                  5 },
    { "forest",   { 0x002010, 0x006020, 0x40a020, 0xc0e060, 0xffffc0 },
                  5 },
    { "mono",     { 0x202020, 0xffffff }, 2 },
};
#define NPALETTES ((int)(sizeof(palettes) / sizeof(palettes[0])))

/* ---------------------------------------------------------------- */
/* Options                                                           */
/* ---------------------------------------------------------------- */

enum {
    P_KIND, P_A, P_B, P_C, P_D, P_RANDOM, P_PALETTE, P_FPS, P_NPARAM
};

static struct gfx_opt par[P_NPARAM] = {
    [P_KIND]    = GFX_OPT_STR('t', "attractor", "clifford"),
    [P_A]       = GFX_OPT_STR('a', "parameter a", 0),
    [P_B]       = GFX_OPT_STR('b', "parameter b", 0),
    [P_C]       = GFX_OPT_STR('c', "parameter c", 0),
    [P_D]       = GFX_OPT_STR('d', "parameter d", 0),
    [P_RANDOM]  = GFX_OPT_FLAG('r', "start from random parameters"),
    [P_PALETTE] = GFX_OPT_NUM('C', "palette", 0, 0, NPALETTES - 1,
                              "0 ember, 1 ice, 2 electric, 3 forest, "
                              "4 mono"),
    [P_FPS]     = GFX_OPT_NUM('F', "frames per second", 15, 1, 60, ""),
};

static void more_help(void)
{
    int i, j;

    puts("attractors, and their defaults:\n");
    for (i = 0; i < NKINDS; i++) {
        const struct kind *k = &kinds[i];

        puts("  ");
        puts(k->name);
        puts("  ");
        puts(k->what);
        puts("\n     ");
        for (j = 0; j < (k->flow ? 4 : k->np); j++) {
            putch(' ');
            putch((char)('a' + j));
            putch('=');
            gfx_put_real(k->def[j], 4);
        }
        putch('\n');
    }
    puts("keys: r random, t next attractor, arrows nudge a and b, "
         "c palette,\n      z clear, p print the command for this one, "
         "space pause, q stop\n");
}

/* ---------------------------------------------------------------- */
/* The picture                                                       */
/* ---------------------------------------------------------------- */

static struct gfx g;
static int W, H;
static u32 *hist;               /* hits per pixel                     */
static u32 hits_max;
static double points;           /* iterated since the last clear      */

static const struct kind *kind;
static double prm[4];
static double st[3];            /* the orbit's state                  */
static double ox, oy, sc;       /* screen = (v - o) * sc              */

static void set_palette(int n)
{
    const struct palette *pl = &palettes[n];

    gfx_colour(&g, COL_BG, 0);
    gfx_ramp(&g, COL_FIRST, NLEVELS, pl->keys, pl->nkeys, 0);
}

static void clear(void)
{
    memset(hist, 0, (u32)W * (u32)H * sizeof(u32));
    hits_max = 0;
    points = 0;
}

static void start_state(void)
{
    st[0] = 0.1 + (double)(gfx_rand() & 0xffff) / 655360.0;
    st[1] = 0.1;
    st[2] = 0.1;
}

/*
 * Find where the attractor lives and fit it to the screen, keeping its
 * proportions. 0 if the orbit runs off to infinity or collapses.
 */
static int fit(void)
{
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30, sx, sy;
    int i, warm = kind->flow ? 4000 : 1000, n = kind->flow ? 60000 : 30000;

    start_state();
    for (i = 0; i < warm; i++) {
        kind->step(st, prm);
        if (!finite(st[0]) || !finite(st[1]) || !finite(st[2])) {
            return 0;
        }
    }
    for (i = 0; i < n; i++) {
        double x, y;

        kind->step(st, prm);
        x = st[kind->px];
        y = st[kind->py];
        if (!finite(x) || !finite(y)) {
            return 0;
        }
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (x1 - x0 < 1e-9 || y1 - y0 < 1e-9) {
        return 0;
    }
    /* Five per cent of margin all round. */
    sx = W * 0.9 / (x1 - x0);
    sy = H * 0.9 / (y1 - y0);
    sc = sx < sy ? sx : sy;
    ox = (x0 + x1) / 2 - W / 2.0 / sc;
    oy = (y0 + y1) / 2 - H / 2.0 / sc;
    return 1;
}

/*
 * Is it chaotic? A map's largest Lyapunov exponent: follow a second
 * orbit 1e-7 away, measure how much the gap grows each step, and put it
 * back to 1e-7 in the same direction. A positive average is sensitive
 * dependence -- chaos. A flow is judged by `spread` alone, since its
 * exponent per step is small and needs a far longer run to measure.
 */
static int chaotic(void)
{
    double s[3], e[3], sum = 0;
    int i;

    if (kind->flow) {
        return 1;
    }
    start_state();
    for (i = 0; i < 1000; i++) {
        kind->step(st, prm);
        if (!finite(st[0]) || !finite(st[1])) {
            return 0;
        }
    }
    s[0] = st[0]; s[1] = st[1]; s[2] = 0;
    e[0] = s[0] + 1e-7; e[1] = s[1]; e[2] = 0;
    for (i = 0; i < 2000; i++) {
        double dx, dy, d;

        kind->step(s, prm);
        kind->step(e, prm);
        dx = e[0] - s[0];
        dy = e[1] - s[1];
        d = sqrt(dx * dx + dy * dy);
        if (!finite(s[0]) || !finite(s[1]) || !(d > 0) || !finite(d)) {
            return 0;
        }
        sum += log(d / 1e-7);
        e[0] = s[0] + dx * (1e-7 / d);
        e[1] = s[1] + dy * (1e-7 / d);
    }
    return sum / 2000 > 0.01;
}

/* Does it cover enough of the screen to be a shape, not a curve? */
static int spread(void)
{
    static u8 grid[32 * 24];
    int i, cells = 0;

    memset(grid, 0, sizeof(grid));
    for (i = 0; i < 20000; i++) {
        int gx, gy;

        kind->step(st, prm);
        gx = (int)((st[kind->px] - ox) * sc) * 32 / W;
        gy = (int)((st[kind->py] - oy) * sc) * 24 / H;
        if (gx >= 0 && gx < 32 && gy >= 0 && gy < 24 && !grid[gy * 32 + gx]) {
            grid[gy * 32 + gx] = 1;
            cells++;
        }
    }
    return cells * 100 >= 32 * 24 * 6;          /* six per cent or more */
}

/* A new attractor of the current kind; the number of tries it took, or
 * 0 if none turned up. */
static int search(void)
{
    int tries, i;

    for (tries = 1; tries <= 2000; tries++) {
        for (i = 0; i < 4; i++) {
            double f = (double)(gfx_rand() & 0xffffff) / 16777216.0;

            prm[i] = kind->lo[i] + (kind->hi[i] - kind->lo[i]) * f;
        }
        if (chaotic() && fit() && spread()) {
            return tries;
        }
    }
    return 0;
}

static void describe(void)
{
    int j;

    puts("attractor -t ");
    puts(kind->name);
    for (j = 0; j < (kind->flow ? 4 : kind->np); j++) {
        puts(" -");
        putch((char)('a' + j));
        putch(' ');
        gfx_put_real(prm[j], 6);
    }
    putch('\n');
}

/* Iterate until `budget` ticks have gone, counting hits. */
static void iterate(u32 budget)
{
    u32 t0 = times(0);
    int i;

    do {
        for (i = 0; i < 1000; i++) {
            int x, y;

            kind->step(st, prm);
            x = (int)((st[kind->px] - ox) * sc);
            y = H - 1 - (int)((st[kind->py] - oy) * sc);
            if (x >= 0 && x < W && y >= 0 && y < H) {
                u32 *h = &hist[(u32)y * (u32)W + (u32)x];

                if (*h != 0xffffffffUL) {
                    ++*h;
                    if (*h > hits_max) {
                        hits_max = *h;
                    }
                }
            }
        }
        points += 1000;
        /* An orbit that has escaped starts again rather than drawing
         * nothing for the rest of the run. */
        if (!finite(st[0]) || !finite(st[1]) || !finite(st[2])) {
            start_state();
        }
    } while (times(0) - t0 < budget);
}

/* log2 of v in Q8: the leading bit's place, and the eight bits after
 * it as the fraction. Integer only, so the redraw costs no FPU. */
static u32 log2q8(u32 v)
{
    u32 n = 31 - (u32)__builtin_clz(v);
    u32 frac = n >= 8 ? (v >> (n - 8)) & 255 : (v << (8 - n)) & 255;

    return n * 256 + frac;
}

static void render(void)
{
    u8 *draw = gfx_frame(&g);
    u32 lmax = hits_max ? log2q8(hits_max) : 1;
    int x, y;

    if (lmax == 0) {
        lmax = 1;
    }
    for (y = 0; y < H; y++) {
        const u32 *h = hist + (u32)y * (u32)W;
        u8 *row = draw + (u32)y * g.info.pitch;

        for (x = 0; x < W; x++) {
            u32 c = h[x];

            row[x] = c ? (u8)(COL_FIRST + log2q8(c) * (NLEVELS - 1) / lmax)
                       : (u8)COL_BG;
        }
    }
    gfx_flip(&g);
}

/* ---------------------------------------------------------------- */

static int find_kind(const char *name)
{
    int i;

    for (i = 0; i < NKINDS; i++) {
        if (strcmp(name, kinds[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

/* The parameters from -a..-d, where given, over the kind's defaults. */
static int take_params(void)
{
    int j;

    for (j = 0; j < 4; j++) {
        const char *s = par[P_A + j].str;

        prm[j] = kind->def[j];
        if (s && gfx_real(s, &prm[j]) < 0) {
            char opt[3] = { '-', (char)('a' + j), '\0' };

            eputs("attractor: ");
            eputs(opt);
            eputs(" wants a number\n");
            return -1;
        }
    }
    return 0;
}

/* After any change: clear, refit, say what it is. 0 if it will not
 * draw. */
static int restart(const char *why)
{
    clear();
    if (!fit()) {
        return 0;
    }
    start_state();
    puts("attractor: ");
    puts(why);
    describe();
    return 1;
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    u32 seed, budget;
    int k, r, kn, pal;

    r = gfx_options(argc, argv, "attractor", par, P_NPARAM, more_help);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    kn = find_kind(par[P_KIND].str);
    if (kn < 0) {
        eputs("attractor: no attractor called ");
        eputs(par[P_KIND].str);
        eputs(" -- attractor -h lists them\n");
        return 2;
    }
    kind = &kinds[kn];
    if (take_params() < 0) {
        return 2;
    }

    if (gfx_open(&g, "attractor", GFX_MAP) < 0) {
        return 1;
    }
    W = (int)g.info.width;
    H = (int)g.info.height;
    hist = malloc((u32)W * (u32)H * sizeof(u32));
    if (!hist) {
        gfx_close(&g);
        eputs("attractor: not enough memory\n");
        return 1;
    }
    seed = gfx_seed();
    pal = (int)par[P_PALETTE].value;
    set_palette(pal);

    if (par[P_RANDOM].value) {
        if (!search()) {
            gfx_close(&g);
            eputs("attractor: no chaotic parameters turned up\n");
            return 1;
        }
    }
    if (!restart("")) {
        gfx_close(&g);
        eputs("attractor: those parameters run off to infinity or "
              "settle to a point\n");
        return 1;
    }
    puts("attractor: seed ");
    putdec(seed);
    puts("; r random, t next, arrows nudge, c palette, z clear, "
         "p print, space pause, q stop\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    /* Most of each frame iterating; the redraw is the rest. */
    budget = HZ * 6 / (10 * par[P_FPS].value);
    if (budget < 1) {
        budget = 1;
    }

    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case 'r': {
            double keep[4];
            int tries, j;

            for (j = 0; j < 4; j++) keep[j] = prm[j];
            tries = search();
            if (tries && restart("")) {
                puts("attractor: found in ");
                putdec((u32)tries);
                puts(tries == 1 ? " try\n" : " tries\n");
            } else {
                puts("attractor: nothing chaotic turned up; keeping this "
                     "one\n");
                for (j = 0; j < 4; j++) prm[j] = keep[j];
                restart("");
            }
            break;
        }
        case 't':
            kn = (kn + 1) % NKINDS;
            kind = &kinds[kn];
            {
                int j;

                for (j = 0; j < 4; j++) prm[j] = kind->def[j];
            }
            if (!restart("")) {
                puts("attractor: ");
                puts(kind->name);
                puts("'s defaults settle to a point or run away here; "
                     "r looks for parameters that do not\n");
            }
            break;
        case GFX_KEY_UP:
        case GFX_KEY_DOWN:
        case GFX_KEY_RIGHT:
        case GFX_KEY_LEFT: {
            /* One per cent of the parameter's search range. */
            int j = (k == GFX_KEY_UP || k == GFX_KEY_DOWN) ? 0 : 1;
            double step = (kind->hi[j] - kind->lo[j]) / 100;
            double was = prm[j];

            if (step == 0) {
                break;
            }
            prm[j] += (k == GFX_KEY_UP || k == GFX_KEY_RIGHT) ? step : -step;
            if (!restart("")) {
                prm[j] = was;
                restart("");
            }
            break;
        }
        case 'c':
            pal = (pal + 1) % NPALETTES;
            set_palette(pal);
            break;
        case 'z':
            clear();
            break;
        case 'p':
            describe();
            break;
        case ' ': {
            u32 at = gfx_clock_pause(&clk);

            while (!gfx_quit_key(k = gfx_key_wait()) && k != ' ') {
                if (k == 'p') {
                    describe();
                }
            }
            gfx_clock_resume(&clk, at);
            if (gfx_quit_key(k)) {
                goto done;
            }
            break;
        }
        }

        iterate(budget);
        render();
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);

    describe();
    gfx_clock_summary(&clk, "attractor", "frames");
    puts(", ");
    putdec((u32)(points / 1000));
    puts(" thousand points in the last picture\n");
    return 0;
}
