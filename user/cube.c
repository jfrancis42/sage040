/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * cube.c - a rotating wireframe cube, as a program.
 *
 *     sage$ cube [frames-per-second]
 *
 * Loaded from the disk by name, run, and back to the shell when you
 * press a key.
 *
 * It touches no hardware. The screen is /dev/fb0, opened and drawn
 * through ioctls; the frame rate comes from nanosleep() against the
 * kernel's tick; the keyboard is asked through ioctl(FIONREAD). There is
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
#include "ulib.h"

/* ---------------------------------------------------------------- */
/* Geometry                                                          */
/* ---------------------------------------------------------------- */

#define HALF        64          /* cube half-edge, object units      */
#define SCALE       1800        /* projection scale                  */
#define DIST        1000        /* camera distance, object units     */
#define FRAC        12          /* sine table fixed point: 4096 = 1  */
#define ONE         (1 << FRAC)

#define COL_BG      0
#define COL_EDGE    1           /* green, in the default palette     */

/*
 * Quarter-turn of sine in Q12: sin(i * 2pi / 256) for i = 0..64. The
 * rest of the circle comes from symmetry. Checked in rather than
 * computed, so the program needs no floating point at all -- the FPU is
 * there, but a cube does not need it and integers are what a machine of
 * this size would have used.
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

    s = isin(ax); c = icos(ax);
    t = (y * c - z * s) >> FRAC;
    z = (y * s + z * c) >> FRAC;
    y = t;

    s = isin(ay); c = icos(ay);
    t = (x * c + z * s) >> FRAC;
    z = (z * c - x * s) >> FRAC;
    x = t;

    s = isin(az); c = icos(az);
    t = (x * c - y * s) >> FRAC;
    y = (x * s + y * c) >> FRAC;
    x = t;

    *ox = x; *oy = y; *oz = z;
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

static int fb = -1;
static struct fb_info info;
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

static void draw_line(int x0, int y0, int x1, int y1, u32 colour)
{
    struct fb_line l;

    l.x0 = x0; l.y0 = y0;
    l.x1 = x1; l.y1 = y1;
    l.colour = colour;
    ioctl(fb, FBIO_LINE, (u32)&l);
}

static void render(int ax, int ay, int az)
{
    int i, rx, ry, rz;

    ioctl(fb, FBIO_CLEAR, COL_BG);

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
            draw_line(sx[edge[i][0]], sy[edge[i][0]],
                      sx[edge[i][1]], sy[edge[i][1]], COL_EDGE);
        }
    }

    ioctl(fb, FBIO_FLIP, 0);
}

static u32 parse_u32(const char *s, u32 fallback)
{
    u32 v = 0;
    int any = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s - '0');
        s++;
        any = 1;
    }
    return (any && *s == '\0') ? v : fallback;
}

int main(int argc, char **argv)
{
    int ax = 0, ay = 0, az = 0;
    u32 frames = 0;
    u32 fps = 50;
    u32 period;
    u32 started;

    if (argc > 1) {
        fps = parse_u32(argv[1], 0);
        if (fps == 0 || fps > 200) {
            eputs("usage: cube [frames-per-second]\n");
            return 2;
        }
    }

    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        eputs("cube: no /dev/fb0\n");
        return 1;
    }
    if (ioctl(fb, FBIO_GETINFO, (u32)&info) < 0) {
        eputs("cube: /dev/fb0 will not say how big it is\n");
        close(fb);
        return 1;
    }
    cx = (int)info.width / 2;
    cy = (int)info.height / 2;

    puts("cube: ");
    putdec(info.width);
    putch('x');
    putdec(info.height);
    putch('x');
    putdec(info.bpp);
    puts(" on ");
    puts(info.name);
    puts(", Q12 fixed point, ");
    putdec(fps);
    puts(" fps\n");
    puts("press any key to stop\n");

    /*
     * Sleep the frame period rather than spinning it out. The kernel has
     * a tick now, so a program can wait without burning the machine --
     * and the rate is the same wherever this runs, which a counted delay
     * loop never was.
     */
    period = 1000 / fps;
    started = times();

    while (!key_waiting()) {
        render(ax, ay, az);

        ax = (ax + 1) & 255;
        ay = (ay + 2) & 255;
        az = (az + 1) & 255;
        frames++;

        msleep(period);
    }

    /* Take the keystroke that stopped it, so it does not turn up at the
     * shell prompt as a stray command. */
    {
        char c;

        read(STDIN_FILENO, &c, 1);
    }

    /* Leave a blank screen rather than the last frame frozen on it, so
     * the display does not look like a machine that has hung. */
    ioctl(fb, FBIO_CLEAR, COL_BG);
    ioctl(fb, FBIO_FLIP, 0);
    close(fb);

    {
        u32 elapsed = times() - started;

        puts("cube: ");
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
