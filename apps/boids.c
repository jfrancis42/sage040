/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * boids.c - Craig Reynolds' flocking model (SIGGRAPH 1987), as a program.
 *
 *     sage$ boids [options]      (boids -h lists them, with defaults)
 *
 * Each boid steers by three rules, looking only at the flockmates it
 * can see -- those within its vision radius and inside its field of
 * view, which leaves a blind spot behind it:
 *
 *   separation   steer away from any that are too close
 *   alignment    steer toward the average heading of the rest
 *   cohesion     steer toward their average position
 *
 * Nothing is told to flock. Flocks, splits and merges come out of those
 * three rules alone, which is the whole point of the model.
 *
 * The steering is Reynolds' own formulation from "Steering Behaviors
 * for Autonomous Characters" (1999): each rule names a DESIRED velocity
 * at full speed, the steering force is that minus the current velocity,
 * and the force is capped. The cap is what makes a boid bank round
 * instead of turning on the spot.
 *
 * Keys: space pauses, r scatters the flock again, w switches wrapping
 * at the edges, c the colouring, and q stops.
 *
 * Like cube, it touches no hardware -- /dev/fb0 through ioctls, the
 * frame rate from msleep(), the keys from standard input, all by way of
 * lib/gfx.c -- and it uses no floating point. Positions and velocities
 * are Q8 pixels; distances are compared in Q4, which keeps every square
 * inside 32 bits.
 *
 * Speeds and forces are PER FRAME, so a machine that cannot keep up
 * with the asked-for frame rate shows a slower flock, not a different
 * one. The summary at exit says what rate was actually achieved.
 */
#include "gfx.h"

/* ---------------------------------------------------------------- */
/* Fixed point                                                       */
/* ---------------------------------------------------------------- */

#define FIX         8           /* positions and velocities: Q8 px   */
#define DFIX        4           /* distances are compared in Q4      */
#define FRAC        GFX_FRAC    /* sine table fixed point: 4096 = 1  */

#define MAX_BOIDS   1000

#define COL_BG      0
#define COL_MONO    1           /* green, in the default palette     */
#define PAL_HUES    GFX_PAL_FREE /* 16 hues round the wheel         */
#define NHUES       16

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/*
 * Length of a vector of any size. Both components are shifted down
 * until their squares fit, and the answer shifted back up; the sums
 * this is used on (a whole neighbourhood's offsets) can be large, and
 * a square that wraps gives a length that is simply wrong.
 */
static int vlen(int x, int y, int *shift)
{
    u32 ax = (u32)iabs(x), ay = (u32)iabs(y);
    int s = 0;

    while (ax > 32767 || ay > 32767) {
        ax >>= 1;
        ay >>= 1;
        s++;
    }
    if (shift) {
        *shift = s;
        return (int)gfx_isqrt(ax * ax + ay * ay);
    }
    return (int)(gfx_isqrt(ax * ax + ay * ay) << s);
}

/* Scale (x, y) to length m. m must be under 65536; a zero vector stays
 * zero, because it has no direction to keep. */
static void setmag(int *x, int *y, int m)
{
    int s, len = vlen(*x, *y, &s);

    if (len == 0) {
        return;
    }
    *x = ((*x >> s) * m) / len;
    *y = ((*y >> s) * m) / len;
}

static void limit(int *x, int *y, int m)
{
    if (vlen(*x, *y, 0) > m) {
        setmag(x, y, m);
    }
}

/* Angle of (x, y) in 256ths of a turn. Good to a unit in the first
 * octant, which is far finer than the sixteen colours it chooses. */
static int iatan2(int y, int x)
{
    int ax = iabs(x), ay = iabs(y);
    int mx = ax > ay ? ax : ay;
    int mn = ax > ay ? ay : ax;
    int a = 0;

    if (mx == 0) {
        return 0;
    }
    /* Shift down so gfx_sin(a) * mx cannot overflow. */
    while (mx > 32767) {
        mx >>= 1;
        mn >>= 1;
    }
    while (a < 32 && gfx_sin(a + 1) * mx <= mn * gfx_cos(a + 1)) {
        a++;
    }
    if (ay > ax) a = 64 - a;
    if (x < 0)   a = 128 - a;
    if (y < 0)   a = 256 - a;
    return a & 255;
}

/* ---------------------------------------------------------------- */
/* Parameters                                                        */
/* ---------------------------------------------------------------- */

/*
 * Everything a person can change, in the units they give it. Speeds are
 * tenths of a pixel per frame and forces hundredths, because whole
 * pixels are far too coarse for either; weights are percentages.
 */
enum {
    P_COUNT, P_RADIUS, P_SEPDIST, P_SEPW, P_ALIGNW, P_COHW,
    P_MAXSPEED, P_MINSPEED, P_FORCE, P_FOV, P_MARGIN, P_TURN,
    P_SIZE, P_COLOUR, P_FPS, P_WRAP, P_NPARAM
};

static struct gfx_opt par[P_NPARAM] = {
    [P_COUNT]    = GFX_OPT_NUM('n', "number of boids", 80, 1, MAX_BOIDS, ""),
    [P_RADIUS]   = GFX_OPT_NUM('r', "vision radius", 48, 1, 300, "px"),
    [P_SEPDIST]  = GFX_OPT_NUM('d', "separation distance", 16, 0, 300,
                               "px"),
    [P_SEPW]     = GFX_OPT_NUM('S', "separation weight", 150, 0, 1000, "%"),
    [P_ALIGNW]   = GFX_OPT_NUM('A', "alignment weight", 100, 0, 1000, "%"),
    [P_COHW]     = GFX_OPT_NUM('C', "cohesion weight", 80, 0, 1000, "%"),
    [P_MAXSPEED] = GFX_OPT_NUM('v', "maximum speed", 35, 1, 200,
                               "tenths px/frame"),
    [P_MINSPEED] = GFX_OPT_NUM('m', "minimum speed", 15, 0, 200,
                               "tenths px/frame"),
    [P_FORCE]    = GFX_OPT_NUM('f', "maximum steering force", 8, 1, 500,
                               "hundredths px/frame^2"),
    [P_FOV]      = GFX_OPT_NUM('a', "field of view", 270, 1, 360,
                               "degrees"),
    [P_MARGIN]   = GFX_OPT_NUM('e', "edge margin", 50, 0, 200, "px"),
    [P_TURN]     = GFX_OPT_NUM('t', "edge turn force", 20, 0, 500,
                               "hundredths px/frame^2"),
    [P_SIZE]     = GFX_OPT_NUM('z', "boid size", 5, 1, 40, "px"),
    [P_COLOUR]   = GFX_OPT_NUM('c', "colour: 0 mono, 1 heading, 2 crowding",
                               1, 0, 2, ""),
    [P_FPS]      = GFX_OPT_NUM('F', "frames per second", 30, 1, 200, ""),
    [P_WRAP]     = GFX_OPT_FLAG('w',
                       "wrap at the screen edges instead of turning back"),
};

static int wrap;                /* -w, and the w key: a torus        */


/* ---------------------------------------------------------------- */
/* The flock                                                         */
/* ---------------------------------------------------------------- */

struct boid {
    int x, y;                   /* Q8 pixels                         */
    int vx, vy;                 /* Q8 pixels per frame               */
    int speed;                  /* |v|, Q8, kept for drawing         */
    int seen;                   /* flockmates seen last frame        */
};

static struct boid flock[MAX_BOIDS];
static int accx[MAX_BOIDS], accy[MAX_BOIDS];
static int nboids;

/* The parameters, converted once to the units the loop uses. */
static int w8, h8;              /* screen size, Q8                   */
static int radius4, radius4sq;  /* vision radius, Q4, and squared    */
static int sep4sq;              /* separation distance squared, Q4   */
static int maxspeed, minspeed;  /* Q8                                */
static int maxforce, turn;      /* Q8                                */
static int margin8;             /* Q8                                */
static int fov_cos;             /* cos(fov / 2), Q12                 */
static int use_fov;

static void setup(u32 width, u32 height)
{
    int half;

    nboids   = (int)par[P_COUNT].value;
    w8       = (int)width << FIX;
    h8       = (int)height << FIX;
    radius4  = (int)par[P_RADIUS].value << DFIX;
    radius4sq = radius4 * radius4;
    sep4sq   = ((int)par[P_SEPDIST].value << DFIX) *
               ((int)par[P_SEPDIST].value << DFIX);
    maxspeed = ((int)par[P_MAXSPEED].value << FIX) / 10;
    minspeed = ((int)par[P_MINSPEED].value << FIX) / 10;
    maxforce = ((int)par[P_FORCE].value << FIX) / 100;
    turn     = ((int)par[P_TURN].value << FIX) / 100;
    margin8  = (int)par[P_MARGIN].value << FIX;
    if (maxforce < 1) maxforce = 1;
    if (maxspeed < 1) maxspeed = 1;

    /* Half the field of view, in 256ths of a turn. */
    half     = (int)(par[P_FOV].value * 128 / 360);
    fov_cos  = gfx_cos(half);
    use_fov  = par[P_FOV].value < 360;
}

static void scatter(void)
{
    int i;

    for (i = 0; i < nboids; i++) {
        struct boid *b = &flock[i];
        int a = (int)(gfx_rand() & 255);
        int s = minspeed + (int)(gfx_rand() % (u32)(maxspeed - minspeed + 1));

        b->x = (int)(gfx_rand() % (u32)w8);
        b->y = (int)(gfx_rand() % (u32)h8);
        b->vx = (gfx_cos(a) * s) >> FRAC;
        b->vy = (gfx_sin(a) * s) >> FRAC;
        b->speed = s;
        b->seen = 0;
    }
}

/*
 * Reynolds' steering: head for `desired` at full speed, and let the
 * force that does it be no more than maxforce.
 */
static void steer(int dx, int dy, const struct boid *b, int *ox, int *oy)
{
    setmag(&dx, &dy, maxspeed);
    dx -= b->vx;
    dy -= b->vy;
    limit(&dx, &dy, maxforce);
    *ox = dx;
    *oy = dy;
}

/* The offset from a to b, the short way round when the screen wraps. */
static void offset(const struct boid *a, const struct boid *b,
                   int *dx, int *dy)
{
    *dx = b->x - a->x;
    *dy = b->y - a->y;
    if (wrap) {
        if (*dx >  w8 / 2) *dx -= w8;
        if (*dx < -w8 / 2) *dx += w8;
        if (*dy >  h8 / 2) *dy -= h8;
        if (*dy < -h8 / 2) *dy += h8;
    }
}

/*
 * Work out every boid's steering from the flock as it stands, and only
 * then move any of them. Updating in place would let the boids earlier
 * in the array see where later ones WILL be, and the flock would drift
 * in the direction of the array.
 */
static void think(void)
{
    int i, j;
    int ws = (int)par[P_SEPW].value;
    int wa = (int)par[P_ALIGNW].value;
    int wc = (int)par[P_COHW].value;

    for (i = 0; i < nboids; i++) {
        struct boid *b = &flock[i];
        int n = 0, ns = 0;
        int alx = 0, aly = 0, cox = 0, coy = 0, spx = 0, spy = 0;
        int ax = 0, ay = 0, sx, sy;

        for (j = 0; j < nboids; j++) {
            const struct boid *o = &flock[j];
            int dx, dy, dx4, dy4, d2;

            if (j == i) {
                continue;
            }
            offset(b, o, &dx, &dy);
            dx4 = dx >> (FIX - DFIX);
            dy4 = dy >> (FIX - DFIX);
            /* A box test first: most of the flock is nowhere near, and
             * this rejects it without a multiply. */
            if (iabs(dx4) > radius4 || iabs(dy4) > radius4) {
                continue;
            }
            d2 = dx4 * dx4 + dy4 * dy4;
            if (d2 > radius4sq) {
                continue;
            }

            /*
             * The blind spot: o is seen if the angle between b's heading
             * and the direction to o is within half the field of view,
             * i.e. v.d >= |v||d| cos(fov/2). The right-hand side is
             * shifted down 12 bits in two halves so that it cannot
             * overflow at the largest radius and speed allowed.
             */
            if (use_fov && b->speed > 0) {
                int dot = b->vx * dx4 + b->vy * dy4;
                int dl = (int)gfx_isqrt((u32)d2);
                int rhs = ((b->speed * dl) >> 6) * fov_cos >> 6;

                if (dot < rhs) {
                    continue;
                }
            }

            n++;
            alx += o->vx;
            aly += o->vy;
            cox += dx;
            coy += dy;

            /* Away from o, weighted by 1/distance: the unit vector is
             * d/|d|, and dividing by |d| again is d/|d|^2. */
            if (d2 < sep4sq) {
                if (d2 == 0) {
                    d2 = 1;
                }
                spx -= (dx4 << 10) / d2;
                spy -= (dy4 << 10) / d2;
                ns++;
            }
        }

        b->seen = n;
        if (n > 0) {
            /* The sums point the same way as the averages would, and
             * steer() only uses the direction. */
            steer(alx, aly, b, &sx, &sy);
            ax += sx * wa;
            ay += sy * wa;
            steer(cox, coy, b, &sx, &sy);
            ax += sx * wc;
            ay += sy * wc;
        }
        if (ns > 0 && (spx || spy)) {
            steer(spx, spy, b, &sx, &sy);
            ax += sx * ws;
            ay += sy * ws;
        }
        ax /= 100;
        ay /= 100;

        /* Reynolds' "containment": turn back from the edges gradually,
         * which is what keeps the flock on screen without a wall. */
        if (!wrap) {
            if (b->x < margin8)      ax += turn;
            if (b->x > w8 - margin8) ax -= turn;
            if (b->y < margin8)      ay += turn;
            if (b->y > h8 - margin8) ay -= turn;
        }
        accx[i] = ax;
        accy[i] = ay;
    }
}

static void move(void)
{
    int i;

    for (i = 0; i < nboids; i++) {
        struct boid *b = &flock[i];

        b->vx += accx[i];
        b->vy += accy[i];
        b->speed = vlen(b->vx, b->vy, 0);
        if (b->speed > maxspeed) {
            setmag(&b->vx, &b->vy, maxspeed);
            b->speed = maxspeed;
        } else if (b->speed < minspeed) {
            if (b->speed == 0) {
                /* Stopped dead: no direction to speed up along, so
                 * pick one. */
                int a = (int)(gfx_rand() & 255);

                b->vx = gfx_cos(a);
                b->vy = gfx_sin(a);
            }
            setmag(&b->vx, &b->vy, minspeed);
            b->speed = minspeed;
        }

        b->x += b->vx;
        b->y += b->vy;
        if (wrap) {
            while (b->x < 0)   b->x += w8;
            while (b->x >= w8) b->x -= w8;
            while (b->y < 0)   b->y += h8;
            while (b->y >= h8) b->y -= h8;
        } else {
            /* With the margin or the turn force set to 0 nothing else
             * stops a boid leaving for good. Bounce as a backstop. */
            if (b->x < 0)   { b->x = 0;      b->vx =  iabs(b->vx); }
            if (b->x >= w8) { b->x = w8 - 1; b->vx = -iabs(b->vx); }
            if (b->y < 0)   { b->y = 0;      b->vy =  iabs(b->vy); }
            if (b->y >= h8) { b->y = h8 - 1; b->vy = -iabs(b->vy); }
        }
    }
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

static struct gfx g;

/*
 * Sixteen hues round the colour wheel. Not full saturation -- the
 * driver's own palette never goes below 0x40 either, and pure primaries
 * on black look harsh.
 */
static const u32 hue_keys[6] = {
    0xff4040UL, 0xffff40UL, 0x40ff40UL, 0x40ffffUL, 0x4040ffUL, 0xff40ffUL
};

static void make_palette(void)
{
    gfx_ramp(&g, PAL_HUES, NHUES, hue_keys, 6, 1);
}

static u32 boid_colour(const struct boid *b)
{
    int c;

    switch (par[P_COLOUR].value) {
    case 0:
        return COL_MONO;
    case 2:
        /* Crowding: a boid alone is blue, one in the thick of it red.
         * Hue 10 is blue, 0 red; eight flockmates is "thick". */
        c = b->seen >= 8 ? 0 : 10 - b->seen * 10 / 8;
        return (u32)(PAL_HUES + c);
    default:
        return (u32)(PAL_HUES + (iatan2(b->vy, b->vx) >> 4));
    }
}

/*
 * A boid is a narrow triangle pointing where it is going: the nose
 * `size` ahead of its position, the two back corners half that behind
 * and 0.4 of it either side.
 */
static void render(void)
{
    int i;
    int size = (int)par[P_SIZE].value;

    gfx_frame(&g);
    gfx_clear(&g, COL_BG);

    for (i = 0; i < nboids; i++) {
        const struct boid *b = &flock[i];
        int px = b->x >> FIX, py = b->y >> FIX;
        int ux, uy, tx, ty, bx, by, wx, wy;
        u32 col;

        if (b->speed == 0) {
            continue;
        }
        /* Unit heading, Q8. */
        ux = (b->vx << FIX) / b->speed;
        uy = (b->vy << FIX) / b->speed;

        tx = px + ((ux * size) >> FIX);
        ty = py + ((uy * size) >> FIX);
        bx = px - ((ux * size) >> (FIX + 1));
        by = py - ((uy * size) >> (FIX + 1));
        wx = (-uy * size * 2 / 5) >> FIX;
        wy = ( ux * size * 2 / 5) >> FIX;

        col = boid_colour(b);
        gfx_line(&g, tx, ty, bx + wx, by + wy, col);
        gfx_line(&g, bx + wx, by + wy, bx - wx, by - wy, col);
        gfx_line(&g, bx - wx, by - wy, tx, ty, col);
    }

    gfx_flip(&g);
}

static void banner(u32 seed)
{
    puts("boids: ");
    putdec((u32)nboids);
    puts(" boids on ");
    putdec(g.info.width);
    putch('x');
    putdec(g.info.height);
    puts(", seed ");
    putdec(seed);
    puts(", ");
    putdec(par[P_FPS].value);
    puts(" fps");
    if (wrap) {
        puts(", wrapping");
    }
    puts("\nspace pauses, r scatters, w wraps, c colours, q stops\n");
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    u32 seed;
    int k, r;

    r = gfx_options(argc, argv, "boids", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (par[P_MINSPEED].value > par[P_MAXSPEED].value) {
        eputs("boids: minimum speed is above the maximum\n");
        return 2;
    }
    wrap = (int)par[P_WRAP].value;

    if (gfx_open(&g, "boids", 0) < 0) {
        return 1;
    }
    seed = gfx_seed();
    setup(g.info.width, g.info.height);
    make_palette();
    scatter();
    banner(seed);

    gfx_clock_start(&clk, par[P_FPS].value);

    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case 'r':
            scatter();
            break;
        case 'w':
            wrap = !wrap;
            break;
        case 'c':
            par[P_COLOUR].value = (par[P_COLOUR].value + 1) % 3;
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

        think();
        move();
        render();
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);

    gfx_clock_summary(&clk, "boids", "frames");
    putch('\n');
    return 0;
}
