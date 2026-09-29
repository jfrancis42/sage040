/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * cube.c - a rotating wireframe cube, as a program.
 *
 *     sage$ cube [-F frames-per-second]
 *
 * Loaded from the disk by name, run, and back to the shell on q or
 * Escape. The arrows change how fast it turns; space pauses.
 *
 * It touches no hardware. The screen is /dev/fb0, opened and drawn
 * through ioctls (lib/gfx.c); the frame rate comes from nanosleep()
 * against the kernel's tick; the keys come from standard input. There is
 * no #include of the machine's hardware header anywhere in this file,
 * which is the thing worth checking if it is ever edited -- an earlier
 * version of this program wrote to the SM501 directly, and could,
 * because with no MMU nothing stops it.
 *
 * It is a smaller relative of ../cube/, which stays on bare metal
 * because its job is to benchmark the 2D engine against the CPU and to
 * drive the MFP's timers directly. That one is a hardware test. This one
 * is an application.
 */
#include "gfx.h"

/* ---------------------------------------------------------------- */
/* Geometry                                                          */
/* ---------------------------------------------------------------- */

#define HALF        64          /* cube half-edge, object units      */
#define SCALE       1800        /* projection scale                  */
#define DIST        1000        /* camera distance, object units     */
#define FRAC        GFX_FRAC    /* sine table fixed point: 4096 = 1  */
#define ONE         GFX_ONE

#define COL_BG      0
#define COL_EDGE    1           /* green, in the default palette     */

/* Cube vertices: bit 0 = +X, bit 1 = +Y, bit 2 = +Z. */
static const signed char vtx[8][3] = {
    { -1, -1, -1 }, { +1, -1, -1 }, { -1, +1, -1 }, { +1, +1, -1 },
    { -1, -1, +1 }, { +1, -1, +1 }, { -1, +1, +1 }, { +1, +1, +1 },
};

/* Face outward normals, in the order +X, -X, +Y, -Y, +Z, -Z. */
static const signed char face_n[6][3] = {
    { +1, 0, 0 }, { -1, 0, 0 },
    { 0, +1, 0 }, { 0, -1, 0 },
    { 0, 0, +1 }, { 0, 0, -1 },
};

/* Twelve edges: two vertices, then the two faces the edge joins. An
 * edge is drawn when either of its faces is turned toward the viewer,
 * which is what hides the three at the back. */
static const unsigned char edge[12][4] = {
    { 0, 1, 3, 5 }, { 2, 3, 2, 5 }, { 4, 5, 3, 4 }, { 6, 7, 2, 4 },
    { 0, 2, 1, 5 }, { 1, 3, 0, 5 }, { 4, 6, 1, 4 }, { 5, 7, 0, 4 },
    { 0, 4, 1, 3 }, { 1, 5, 0, 3 }, { 2, 6, 1, 2 }, { 3, 7, 0, 2 },
};

static int sx[8], sy[8];
static int visible[6];

static void rotate(int x, int y, int z, int ax, int ay, int az,
                   int *ox, int *oy, int *oz)
{
    int s, c, t;

    s = gfx_sin(ax); c = gfx_cos(ax);
    t = (y * c - z * s) >> FRAC;
    z = (y * s + z * c) >> FRAC;
    y = t;

    s = gfx_sin(ay); c = gfx_cos(ay);
    t = (x * c + z * s) >> FRAC;
    z = (z * c - x * s) >> FRAC;
    x = t;

    s = gfx_sin(az); c = gfx_cos(az);
    t = (x * c - y * s) >> FRAC;
    y = (x * s + y * c) >> FRAC;
    x = t;

    *ox = x; *oy = y; *oz = z;
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

static struct gfx g;
static int cx, cy;

/*
 * +Z points AT the viewer, the same convention the backface test uses.
 * So a vertex with larger z is NEARER, and its distance from the camera
 * is DIST - z. Getting that backwards does not look broken, it looks
 * like a badly distorted cube -- far corners drawing larger than near
 * ones.
 */
static void project(int x, int y, int z, int *px, int *py)
{
    int d = DIST - z;

    if (d < 1) {
        d = 1;
    }
    *px = cx + (x * SCALE) / d;
    *py = cy - (y * SCALE) / d;
}

static void render(int ax, int ay, int az)
{
    int i, rx, ry, rz;

    gfx_frame(&g);
    gfx_clear(&g, COL_BG);

    /*
     * Which faces point at the viewer? A face is visible when its
     * rotated normal has positive Z.
     *
     * The normals are scaled to ONE before rotating, and that is not
     * optional. rotate() works in Q12 and shifts its products back down
     * by 12 bits, so a component of 1 becomes 0 the moment it is
     * multiplied by anything less than 4096 -- every face then tests as
     * facing away, no edge is ever drawn, and the screen stays black
     * while the frame counter climbs. Cost me an afternoon.
     */
    for (i = 0; i < 6; i++) {
        rotate(face_n[i][0] * ONE, face_n[i][1] * ONE, face_n[i][2] * ONE,
               ax, ay, az, &rx, &ry, &rz);
        visible[i] = (rz > 0);
    }

    for (i = 0; i < 8; i++) {
        rotate(vtx[i][0] * HALF, vtx[i][1] * HALF, vtx[i][2] * HALF,
               ax, ay, az, &rx, &ry, &rz);
        project(rx, ry, rz, &sx[i], &sy[i]);
    }

    for (i = 0; i < 12; i++) {
        if (visible[edge[i][2]] || visible[edge[i][3]]) {
            gfx_line(&g, sx[edge[i][0]], sy[edge[i][0]],
                     sx[edge[i][1]], sy[edge[i][1]], COL_EDGE);
        }
    }

    gfx_flip(&g);
}

enum { P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_FPS] = GFX_OPT_NUM('F', "frames per second", 50, 1, 200, ""),
};

int main(int argc, char **argv)
{
    int ax = 0, ay = 0, az = 0;
    int dx = 1, dy = 2, dz = 1;         /* turn per frame, 256ths      */
    struct gfx_clock clk;
    int k, r;

    r = gfx_options(argc, argv, "cube", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (gfx_open(&g, "cube", 0) < 0) {
        return 1;
    }

    cx = (int)g.info.width / 2;
    cy = (int)g.info.height / 2;

    puts("cube: ");
    putdec(g.info.width);
    putch('x');
    putdec(g.info.height);
    putch('x');
    putdec(g.info.bpp);
    puts(" on ");
    puts(g.info.name);
    puts(", Q12 fixed point, ");
    putdec(par[P_FPS].value);
    puts(" fps\n");
    puts("arrows turn it faster or slower, space pauses, q stops\n");

    /*
     * Sleep out each frame rather than spinning it out. The kernel has a
     * tick now, so a program can wait without burning the machine -- and
     * the rate is the same wherever this runs, which a counted delay
     * loop never was.
     */
    gfx_clock_start(&clk, par[P_FPS].value);

    for (;;) {
        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case GFX_KEY_UP:    if (dx < 8)  dx++; break;
        case GFX_KEY_DOWN:  if (dx > -8) dx--; break;
        case GFX_KEY_RIGHT: if (dy < 8)  dy++; break;
        case GFX_KEY_LEFT:  if (dy > -8) dy--; break;
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

        render(ax, ay, az);

        ax = (ax + dx) & 255;
        ay = (ay + dy) & 255;
        az = (az + dz) & 255;

        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);

    gfx_clock_summary(&clk, "cube", "frames");
    putch('\n');
    return 0;
}
