/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * voxel.c - flying over a landscape, the way Comanche (NovaLogic, 1992)
 * drew one.
 *
 *     sage$ voxel [options]      (voxel -h lists them)
 *
 * The world is two 256x256 maps that wrap at the edges: a HEIGHT for
 * every point, made by diamond-square midpoint displacement, and a
 * COLOUR, chosen by height (water, sand, grass, forest, rock, snow) and
 * lit by the slope towards the sun.
 *
 * The screen is drawn a column at a time, front to back. For each
 * distance out from the camera, the line across the view at that
 * distance is walked in step with the screen's columns; each map point
 * on it becomes a vertical span, from where its height projects on the
 * screen down to whatever that column has already drawn -- so nearer
 * ground hides what is behind it without a depth buffer, and a column
 * finished early is simply skipped. No polygons, no FPU: additions, one
 * reciprocal per distance, and a multiply per column.
 *
 * It flies on its own. The arrows steer and change speed, a and z climb
 * and dive (it will not fly into the ground), r makes a new world,
 * space pauses, q stops.
 */
#include "gfx.h"
#include "malloc.h"

#define MAP     256
#define MASK    (MAP - 1)

/* The palette: seven bands of 32 shades, from dark to light. */
#define SHADES  32
enum { B_WATER, B_SAND, B_GRASS, B_FOREST, B_ROCK, B_SNOW, B_SKY, NBANDS };

enum { P_CELL, P_FAR, P_ROUGH, P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_CELL]  = GFX_OPT_NUM('z', "pixel size", 4, 1, 8, "px"),
    [P_FAR]   = GFX_OPT_NUM('d', "view distance", 300, 50, 800, "map units"),
    [P_ROUGH] = GFX_OPT_NUM('R', "roughness", 60, 10, 100, "%"),
    [P_FPS]   = GFX_OPT_NUM('F', "frames per second", 30, 0, 200,
                            "0 for as fast as it will go"),
};

static struct gfx g;
static u8 height[MAP * MAP];
static u8 colour[MAP * MAP];
static int rw, rh;              /* the picture, before it is enlarged */
static u8 *img;
static int *ybuf;
static u8 rowbuf[1024] __attribute__((aligned(4)));

/* The camera: position in 24.8 map units, heading in 256ths of a turn,
 * height in map units, speed in 1/256 map units a frame. */
static s32 camx, camy;
static int heading, alt, speed = 256;

static const u32 band_dark[NBANDS] = {
    0x001040UL, 0x806a40UL, 0x1a4a14UL, 0x0c2a0cUL, 0x3a3634UL, 0x8890a0UL,
    0x4070d0UL
};
static const u32 band_light[NBANDS] = {
    0x3080e0UL, 0xf0e0a0UL, 0x70c050UL, 0x2e7a2aUL, 0xa0968cUL, 0xffffffUL,
    0xc0e0ffUL
};

static void make_palette(void)
{
    u32 b, i;

    for (b = 0; b < NBANDS; b++) {
        for (i = 0; i < SHADES; i++) {
            gfx_colour(&g, GFX_PAL_FREE + b * SHADES + i,
                       gfx_blend(band_dark[b], band_light[b], i, SHADES - 1));
        }
    }
}

static u8 ink(int band, int shade)
{
    if (shade < 0) shade = 0;
    if (shade >= SHADES) shade = SHADES - 1;
    return (u8)(GFX_PAL_FREE + band * SHADES + shade);
}

/* ---------------------------------------------------------------- */
/* The world                                                         */
/* ---------------------------------------------------------------- */

static int hv[MAP * MAP];       /* heights while they are being made */

#define H(x, y) hv[((y) & MASK) * MAP + ((x) & MASK)]

static int noise(int amp)
{
    return amp ? (int)(gfx_rand() % (u32)(2 * amp + 1)) - amp : 0;
}

/*
 * Diamond-square on a torus: every midpoint is the average of four
 * points and a random displacement that shrinks with the square, by
 * the roughness each halving. Wrapping the coordinates is what makes
 * the map tile, so the flight never reaches an edge.
 */
static void make_world(void)
{
    int step, amp = 12000, x, y, lo = 1 << 30, hi = -(1 << 30);
    int water = 0;
    u32 rough = par[P_ROUGH].value;

    memset(hv, 0, sizeof(hv));
    for (step = MAP; step > 1; step /= 2) {
        int half = step / 2;

        for (y = 0; y < MAP; y += step) {       /* square: centres */
            for (x = 0; x < MAP; x += step) {
                H(x + half, y + half) = (H(x, y) + H(x + step, y) +
                                         H(x, y + step) +
                                         H(x + step, y + step)) / 4 +
                                        noise(amp);
            }
        }
        for (y = 0; y < MAP; y += half) {       /* diamond: edges */
            for (x = (y / half) % 2 ? 0 : half; x < MAP; x += step) {
                H(x, y) = (H(x - half, y) + H(x + half, y) +
                           H(x, y - half) + H(x, y + half)) / 4 +
                          noise(amp);
            }
        }
        amp = amp * (int)rough / 100;
    }
    for (x = 0; x < MAP * MAP; x++) {
        if (hv[x] < lo) lo = hv[x];
        if (hv[x] > hi) hi = hv[x];
    }
    for (x = 0; x < MAP * MAP; x++) {
        height[x] = (u8)((hv[x] - lo) * 255 / (hi - lo + 1));
    }
    /* A third of the world under water: the sea is flat. */
    {
        u32 count[256], acc = 0;

        memset(count, 0, sizeof(count));
        for (x = 0; x < MAP * MAP; x++) {
            count[height[x]]++;
        }
        for (water = 0; water < 255 && acc < MAP * MAP / 3; water++) {
            acc += count[water];
        }
    }
    for (y = 0; y < MAP; y++) {
        for (x = 0; x < MAP; x++) {
            int i = y * MAP + x, hgt = height[i];
            /* The sun is to the north-west: brighter facing it. */
            int slope = hgt - height[((y - 1) & MASK) * MAP + ((x - 1) & MASK)];
            int band, shade = SHADES / 2 + slope * 2;

            if (hgt < water) {
                height[i] = (u8)water;
                colour[i] = ink(B_WATER, SHADES / 2 + (hgt - water) / 4);
                continue;
            }
            band = hgt < water + 6 ? B_SAND :
                   hgt < water + 45 ? B_GRASS :
                   hgt < water + 90 ? B_FOREST :
                   hgt < 225 ? B_ROCK : B_SNOW;
            colour[i] = ink(band, shade);
        }
    }
}

/* ---------------------------------------------------------------- */
/* The view                                                          */
/* ---------------------------------------------------------------- */

static int ground(s32 x, s32 y)
{
    return height[((y >> 8) & MASK) * MAP + ((x >> 8) & MASK)];
}

static void render(void)
{
    int far = (int)par[P_FAR].value, horizon = rh / 3;
    int s = gfx_sin(heading), c = gfx_cos(heading);   /* Q12 */
    int scale = rh * 2 / 3;                           /* the projection */
    int z = 4, dz = 1, i, y;
    u32 cell = par[P_CELL].value, k, x;
    u8 *draw;

    for (i = 0; i < rw; i++) {
        ybuf[i] = rh;
    }
    while (z < far) {
        /* The line across the view at distance z: from its left end to
         * its right, in rw steps, 24.8. A 90-degree field of view. */
        s32 lx = camx + ((-c * z - s * z) >> 4);
        s32 ly = camy + ((s * z - c * z) >> 4);
        s32 rx = camx + ((c * z - s * z) >> 4);
        s32 ry = camy + ((-s * z - c * z) >> 4);
        s32 stx = (rx - lx) / rw, sty = (ry - ly) / rw;
        int inv = (scale << 8) / z;                   /* 24.8 */
        int fade = z * 12 / far;                      /* darker far off */

        for (i = 0; i < rw; i++) {
            int top;

            if (ybuf[i] > 0) {
                int hgt = ground(lx, ly);

                top = ((alt - hgt) * inv >> 8) + horizon;
                if (top < ybuf[i]) {
                    u32 m = ((u32)(ly >> 8) & MASK) * MAP +
                            ((u32)(lx >> 8) & MASK);
                    u8 cl = colour[m];
                    int shade = (cl - GFX_PAL_FREE) % SHADES - fade;
                    int band = (cl - GFX_PAL_FREE) / SHADES;

                    cl = ink(band, shade);
                    if (top < 0) {
                        top = 0;
                    }
                    for (y = top; y < ybuf[i]; y++) {
                        img[(u32)y * (u32)rw + (u32)i] = cl;
                    }
                    ybuf[i] = top;
                }
            }
            lx += stx;
            ly += sty;
        }
        z += dz;
        if (z > 50) {
            dz = z / 50;                    /* coarser far away */
        }
    }
    /* What is left of each column is sky, lighter at the horizon. */
    for (i = 0; i < rw; i++) {
        for (y = 0; y < ybuf[i]; y++) {
            img[(u32)y * (u32)rw + (u32)i] =
                ink(B_SKY, y * SHADES / (horizon + rh / 6 + 1));
        }
    }

    draw = gfx_frame(&g);
    for (y = 0; y < rh; y++) {
        const u8 *row = img + (u32)y * (u32)rw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)rw; x++) {
            for (k = 0; k < cell; k++) {
                *p++ = row[x];
            }
        }
        for (k = 0; k < cell; k++) {
            gfx_copy_row(draw + ((u32)y * cell + k) * g.info.pitch, rowbuf,
                         (u32)rw * cell);
        }
    }
    gfx_flip(&g);
}

static void fly(void)
{
    int floor;

    camx += (s32)(-gfx_sin(heading) * speed) >> 12;
    camy += (s32)(-gfx_cos(heading) * speed) >> 12;
    /* Never into the ground: stay a little above what is below and a
     * little ahead. */
    floor = ground(camx, camy);
    if (ground(camx - (gfx_sin(heading) << 4), camy - (gfx_cos(heading) << 4)) >
        floor) {
        floor = ground(camx - (gfx_sin(heading) << 4),
                       camy - (gfx_cos(heading) << 4));
    }
    if (alt < floor + 20) {
        alt = floor + 20;
    }
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    u32 seed;
    int k, r;

    r = gfx_options(argc, argv, "voxel", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (gfx_open(&g, "voxel", GFX_MAP) < 0) {
        return 1;
    }
    if (g.info.width > sizeof(rowbuf)) {
        gfx_close(&g);
        eputs("voxel: this screen is wider than it can draw\n");
        return 1;
    }
    rw = (int)(g.info.width / par[P_CELL].value);
    rh = (int)(g.info.height / par[P_CELL].value);
    img = malloc((u32)rw * (u32)rh);
    ybuf = malloc((u32)rw * sizeof(int));
    if (!img || !ybuf) {
        gfx_close(&g);
        eputs("voxel: not enough memory\n");
        return 1;
    }
    seed = gfx_seed();
    make_palette();
    make_world();
    gfx_clear(&g, 0);
    camx = camy = (MAP / 2) << 8;
    alt = ground(camx, camy) + 50;

    puts("voxel: ");
    putdec((u32)rw);
    putch('x');
    putdec((u32)rh);
    puts(", seed ");
    putdec(seed);
    puts("\narrows steer and change speed, a z climb and dive, r new world, "
         "space pauses, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case GFX_KEY_LEFT:  heading = (heading + 4) & 255; break;
        case GFX_KEY_RIGHT: heading = (heading - 4) & 255; break;
        case GFX_KEY_UP:    if (speed < 2048) speed += 64; break;
        case GFX_KEY_DOWN:  if (speed > 0) speed -= 64; break;
        case 'a':           if (alt < 400) alt += 8; break;
        case 'z':           alt -= 8; break;
        case 'r':           make_world(); break;
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
        fly();
        render();
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);
    gfx_clock_summary(&clk, "voxel", "frames");
    putch('\n');
    return 0;
}
