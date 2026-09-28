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
 * Like cube, it touches no hardware -- /dev/fb0 through ioctls, the
 * frame rate from msleep(), the keyboard through FIONREAD -- and it uses
 * no floating point. Positions and velocities are Q8 pixels; distances
 * are compared in Q4, which keeps every square inside 32 bits.
 *
 * Speeds and forces are PER FRAME, so a machine that cannot keep up
 * with the asked-for frame rate shows a slower flock, not a different
 * one. The summary at exit says what rate was actually achieved.
 */
#include "ulib.h"

/* ---------------------------------------------------------------- */
/* Fixed point                                                       */
/* ---------------------------------------------------------------- */

#define FIX         8           /* positions and velocities: Q8 px   */
#define DFIX        4           /* distances are compared in Q4      */
#define FRAC        12          /* sine table fixed point: 4096 = 1  */
#define ONE         (1 << FRAC)

#define MAX_BOIDS   1000

#define COL_BG      0
#define COL_MONO    1           /* green, in the default palette     */
#define PAL_HUES    32          /* our 16 hues start here; the       */
#define NHUES       16          /* console owns 16-31, the driver 0-7 */

/*
 * Quarter-turn of sine in Q12, sin(i * 2pi / 256) for i = 0..64 --
 * cube's table, copied rather than retyped: retyping that maths from
 * memory has broken the cube twice.
 */
static const short sin_q[65] = {
       0,  100,  201,  301,  401,  501,  601,  700,
     799,  897,  995, 1092, 1189, 1285, 1380, 1474,
    1567, 1660, 1751, 1842, 1931, 2019, 2106, 2191,
    2276, 2359, 2440, 2520, 2598, 2675, 2751, 2824,
    2896, 2967, 3035, 3102, 3166, 3229, 3290, 3349,
    3406, 3461, 3513, 3564, 3612, 3659, 3703, 3745,
    3784, 3822, 3857, 3889, 3920, 3948, 3973, 3996,
    4017, 4036, 4052, 4065, 4076, 4085, 4091, 4095,
    4096
};

static int isin(int a)
{
    a &= 255;
    if (a <= 64)  return sin_q[a];
    if (a <= 128) return sin_q[128 - a];
    if (a <= 192) return -sin_q[a - 128];
    return -sin_q[256 - a];
}

static int icos(int a)
{
    return isin(a + 64);
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static u32 isqrt(u32 v)
{
    u32 r = 0, b = 1UL << 30;

    while (b > v) {
        b >>= 2;
    }
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
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
        return (int)isqrt(ax * ax + ay * ay);
    }
    return (int)(isqrt(ax * ax + ay * ay) << s);
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
    /* Shift down so isin(a) * mx cannot overflow. */
    while (mx > 32767) {
        mx >>= 1;
        mn >>= 1;
    }
    while (a < 32 && isin(a + 1) * mx <= mn * icos(a + 1)) {
        a++;
    }
    if (ay > ax) a = 64 - a;
    if (x < 0)   a = 128 - a;
    if (y < 0)   a = 256 - a;
    return a & 255;
}

static u32 rng_state;

/* xorshift32: small, fast, and a seed can be given to repeat a run. */
static u32 rnd(void)
{
    u32 x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* ---------------------------------------------------------------- */
/* Parameters                                                        */
/* ---------------------------------------------------------------- */

/*
 * Everything a person can change, in the units they give it. Speeds are
 * tenths of a pixel per frame and forces hundredths, because whole
 * pixels are far too coarse for either; weights are percentages.
 */
struct param {
    char opt;
    const char *name;
    u32 value;
    u32 min, max;
    const char *unit;
};

enum {
    P_COUNT, P_RADIUS, P_SEPDIST, P_SEPW, P_ALIGNW, P_COHW,
    P_MAXSPEED, P_MINSPEED, P_FORCE, P_FOV, P_MARGIN, P_TURN,
    P_SIZE, P_COLOUR, P_FPS, P_NPARAM
};

static struct param par[P_NPARAM] = {
    [P_COUNT]    = { 'n', "number of boids",           80, 1, MAX_BOIDS, "" },
    [P_RADIUS]   = { 'r', "vision radius",             48, 1, 300, "px" },
    [P_SEPDIST]  = { 'd', "separation distance",       16, 0, 300, "px" },
    [P_SEPW]     = { 'S', "separation weight",        150, 0, 1000, "%" },
    [P_ALIGNW]   = { 'A', "alignment weight",         100, 0, 1000, "%" },
    [P_COHW]     = { 'C', "cohesion weight",           80, 0, 1000, "%" },
    [P_MAXSPEED] = { 'v', "maximum speed",             35, 1, 200,
                     "tenths px/frame" },
    [P_MINSPEED] = { 'm', "minimum speed",             15, 0, 200,
                     "tenths px/frame" },
    [P_FORCE]    = { 'f', "maximum steering force",     8, 1, 500,
                     "hundredths px/frame^2" },
    [P_FOV]      = { 'a', "field of view",            270, 1, 360, "degrees" },
    [P_MARGIN]   = { 'e', "edge margin",               50, 0, 200, "px" },
    [P_TURN]     = { 't', "edge turn force",           20, 0, 500,
                     "hundredths px/frame^2" },
    [P_SIZE]     = { 'z', "boid size",                  5, 1, 40, "px" },
    [P_COLOUR]   = { 'c', "colour: 0 mono, 1 heading, 2 crowding",
                                                        1, 0, 2, "" },
    [P_FPS]      = { 'F', "frames per second",         30, 1, 200, "" },
};

static int wrap;                /* -w: the screen is a torus        */
static u32 seed;                /* -x: 0 means "from the clock"     */

/* To stdout: it is only printed when -h asks for it. */
static void usage(void)
{
    int i;

    puts("usage: boids [-w] [-x seed] [-OPT value ...]\n");
    for (i = 0; i < P_NPARAM; i++) {
        char buf[4] = { ' ', '-', par[i].opt, '\0' };

        puts(buf);
        puts("  ");
        puts(par[i].name);
        if (par[i].unit[0]) {
            puts(", ");
            puts(par[i].unit);
        }
        puts(" (default ");
        putdec(par[i].value);
        puts(")\n");
    }
    puts(" -w  wrap at the screen edges instead of turning back\n");
    puts(" -x  random seed (default: from the clock)\n");
    puts(" -h  this\n");
}

static int parse_u32(const char *s, u32 *out)
{
    u32 v = 0;
    int any = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s - '0');
        s++;
        any = 1;
    }
    if (!any || *s != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

/*
 * -n 80 and -n80 both work. Flags cannot be bundled (-wh): with this
 * many options taking values it would be a way to be misunderstood.
 */
static int parse_args(int argc, char **argv)
{
    int i, k;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val;
        u32 v;

        if (a[0] != '-' || a[1] == '\0') {
            return -1;
        }
        if (a[1] == 'h' && a[2] == '\0') {
            usage();
            return 1;
        }
        if (a[1] == 'w' && a[2] == '\0') {
            wrap = 1;
            continue;
        }

        val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
        if (!val || parse_u32(val, &v) < 0) {
            eputs("boids: -");
            eputs(a + 1);
            eputs(" wants a number\n");
            return -1;
        }
        if (a[1] == 'x') {
            seed = v;
            continue;
        }
        for (k = 0; k < P_NPARAM; k++) {
            if (par[k].opt == a[1]) {
                break;
            }
        }
        if (k == P_NPARAM) {
            eputs("boids: unknown option ");
            eputs(a);
            eputs("\n");
            return -1;
        }
        if (v < par[k].min || v > par[k].max) {
            eputs("boids: ");
            eputs(par[k].name);
            eputs(" out of range\n");
            return -1;
        }
        par[k].value = v;
    }

    if (par[P_MINSPEED].value > par[P_MAXSPEED].value) {
        eputs("boids: minimum speed is above the maximum\n");
        return -1;
    }
    return 0;
}

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
    fov_cos  = icos(half);
    use_fov  = par[P_FOV].value < 360;
}

static void scatter(void)
{
    int i;

    for (i = 0; i < nboids; i++) {
        struct boid *b = &flock[i];
        int a = (int)(rnd() & 255);
        int s = minspeed + (int)(rnd() % (u32)(maxspeed - minspeed + 1));

        b->x = (int)(rnd() % (u32)w8);
        b->y = (int)(rnd() % (u32)h8);
        b->vx = (icos(a) * s) >> FRAC;
        b->vy = (isin(a) * s) >> FRAC;
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
                int dl = (int)isqrt((u32)d2);
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
                int a = (int)(rnd() & 255);

                b->vx = icos(a);
                b->vy = isin(a);
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

static int fb = -1;
static struct fb_info info;

/*
 * Sixteen hues round the colour wheel, at palette entries 32-47: the
 * driver's eight colours are 0-7 and the console's sixteen are 16-31,
 * and both stay as they were. Not full saturation -- the driver's own
 * palette never goes below 0x40 either, and pure primaries on black
 * look harsh.
 */
static void make_palette(void)
{
    int i;

    for (i = 0; i < NHUES; i++) {
        int h = i * 6 * 256 / NHUES;   /* 0..1535: six sectors of 256 */
        int f = h & 255;
        int lo = 0x40, hi = 0xff;
        int up = lo + (hi - lo) * f / 256;
        int dn = hi - (hi - lo) * f / 256;
        int r, g, bl;
        struct fb_palette p;

        switch (h >> 8) {
        case 0:  r = hi; g = up; bl = lo; break;
        case 1:  r = dn; g = hi; bl = lo; break;
        case 2:  r = lo; g = hi; bl = up; break;
        case 3:  r = lo; g = dn; bl = hi; break;
        case 4:  r = up; g = lo; bl = hi; break;
        default: r = hi; g = lo; bl = dn; break;
        }
        p.index = (u32)(PAL_HUES + i);
        p.rgb = ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
        ioctl(fb, FBIO_PALETTE, (u32)&p);
    }
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

static void draw_line(int x0, int y0, int x1, int y1, u32 colour)
{
    struct fb_line l;

    l.x0 = x0; l.y0 = y0;
    l.x1 = x1; l.y1 = y1;
    l.colour = colour;
    ioctl(fb, FBIO_LINE, (u32)&l);
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

    ioctl(fb, FBIO_CLEAR, COL_BG);

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
        draw_line(tx, ty, bx + wx, by + wy, col);
        draw_line(bx + wx, by + wy, bx - wx, by - wy, col);
        draw_line(bx - wx, by - wy, tx, ty, col);
    }

    ioctl(fb, FBIO_FLIP, 0);
}

/*
 * Sleep until frame `n` is due, counting from the tick `*start`.
 *
 * Sleeping a fixed period each frame undershoots by construction: the
 * period is rounded to whole ticks, and the frame's own drawing time
 * comes on top. 30 fps asked for came out as 24. Aiming at a deadline
 * measured from the start instead lets one frame's rounding be made up
 * by the next, so the average is what was asked for. A program that
 * falls more than a frame behind -- the machine was busy -- starts the
 * schedule again from now rather than racing to catch up, which would
 * show as a burst of speed.
 */
static void frame_wait(u32 *start, u32 *n, u32 fps)
{
    u32 due, now;
    s32 ahead;

    (*n)++;
    due = *start + (*n * HZ) / fps;
    now = times(0);
    ahead = (s32)(due - now);
    if (ahead > 0) {
        msleep((u32)ahead * 1000 / HZ);
    } else if (-ahead > (s32)(HZ / fps) + 1) {
        *start = now;
        *n = 0;
    }
}

int main(int argc, char **argv)
{
    u32 frames = 0;
    u32 sched = 0, sched_start;
    u32 started;
    int r;

    r = parse_args(argc, argv);
    if (r > 0) {
        return 0;
    }
    if (r < 0) {
        eputs("boids -h lists the options\n");
        return 2;
    }

    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        eputs("boids: no /dev/fb0\n");
        return 1;
    }
    if (ioctl(fb, FBIO_GETINFO, (u32)&info) < 0) {
        eputs("boids: /dev/fb0 will not say how big it is\n");
        close(fb);
        return 1;
    }
    /* Double buffering is not the framebuffer's default state; whatever
     * ran last left it however it left it (see cube.c). */
    if (ioctl(fb, FBIO_DOUBLE, 1) < 0) {
        eputs("boids: /dev/fb0 cannot double buffer\n");
        close(fb);
        return 1;
    }

    if (seed == 0) {
        seed = (u32)time(0) ^ times(0);
    }
    rng_state = seed ? seed : 1;    /* xorshift is stuck at 0 */

    setup(info.width, info.height);
    make_palette();
    scatter();

    puts("boids: ");
    putdec((u32)nboids);
    puts(" boids on ");
    putdec(info.width);
    putch('x');
    putdec(info.height);
    puts(", seed ");
    putdec(seed);
    puts(", ");
    putdec(par[P_FPS].value);
    puts(" fps");
    if (wrap) {
        puts(", wrapping");
    }
    puts("\npress any key to stop\n");

    started = times(0);
    sched_start = started;

    while (!key_waiting()) {
        think();
        move();
        render();
        frames++;
        frame_wait(&sched_start, &sched, par[P_FPS].value);
    }

    /* Take the keystroke that stopped it, so it does not turn up at the
     * shell prompt as a stray command. */
    {
        char c;

        read(STDIN_FILENO, &c, 1);
    }

    ioctl(fb, FBIO_CLEAR, COL_BG);
    ioctl(fb, FBIO_FLIP, 0);
    close(fb);

    {
        u32 elapsed = times(0) - started;

        puts("boids: ");
        putdec(frames);
        puts(" frames in ");
        putdec(elapsed / 100);
        puts(" seconds");
        if (elapsed > 0) {
            puts(" (");
            putdec(frames * 100 / elapsed);
            puts(" fps)");
        }
        putch('\n');
    }
    return 0;
}
