/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * cube.c - a rotating wireframe cube, as a program.
 *
 *     sage$ cube
 *
 * Loaded from the disk by name, run, and it comes back to the shell when
 * you press a key. That is the whole point of it: it is the first thing
 * on this machine that is a program rather than a kernel.
 *
 * It is a smaller relative of ../cube/, which is a bare-metal demo that
 * also benchmarks the SM501's 2D engine against the CPU and drives the
 * MFP's timers to pace itself. That one is a hardware test and belongs
 * on bare metal. This one is an application: its text goes through
 * write(), it asks the terminal whether a key is waiting through
 * ioctl(), and it leaves through exit().
 *
 * ONE THING IT DOES THAT A PROGRAM SHOULD NOT.
 *
 * It writes to the SM501's registers and video memory directly, which
 * means including the machine's hardware header. There is no framebuffer
 * device yet -- a display is neither a byte stream nor a block device,
 * so it does not fit any of the classes in dev.h, and giving it one is
 * an open item rather than an oversight. Until then this is honest about
 * reaching around the kernel instead of pretending not to.
 *
 * Nothing stops it: with no MMU turned on, a program can write anywhere.
 * The kernel bounds-checks where it *loads* a program, which is a
 * different thing from confining one once it runs.
 */
#include "ulib.h"
#include "sage040.h"

/* ---------------------------------------------------------------- */
/* Screen                                                            */
/* ---------------------------------------------------------------- */

#define SCR_W       640
#define SCR_H       480
#define SCR_BYTES   (SCR_W * SCR_H)

/* Two buffers, a megabyte apart, so a frame is drawn while the other
 * one is on screen. */
#define FB0_OFFSET  0x000000UL
#define FB1_OFFSET  0x100000UL

#define COL_BG      0
#define COL_EDGE    1

/* ---------------------------------------------------------------- */
/* Geometry                                                          */
/* ---------------------------------------------------------------- */

#define HALF        64          /* cube half-edge, object units      */
#define SCALE       1800        /* projection scale                  */
#define DIST        1000        /* camera distance, object units     */
#define CX          (SCR_W / 2)
#define CY          (SCR_H / 2)

#define FRAC        12          /* sine table fixed point: 4096 = 1  */

/*
 * Quarter-turn of sine in Q12: sin(i * 2pi / 256) for i = 0..64. The
 * rest of the circle comes from symmetry. Checked in rather than
 * computed, so the program needs no floating point and no start-up work.
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
 * edge is drawn when either of its faces is turned toward the viewer. */
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

/*
 * +Z points AT the viewer, which is the same convention the backface
 * test uses. So a vertex with larger z is NEARER, and its distance from
 * the camera is DIST - z. Getting that backwards does not look broken,
 * it looks like a badly distorted cube -- far corners drawing larger
 * than near ones.
 */
static void project(int x, int y, int z, int *px, int *py)
{
    int d = DIST - z;

    if (d < 1) {
        d = 1;
    }
    *px = CX + (x * SCALE) / d;
    *py = CY - (y * SCALE) / d;
}

/* ---------------------------------------------------------------- */
/* Drawing                                                           */
/* ---------------------------------------------------------------- */

static u32 back;                /* offset of the buffer being drawn */

/*
 * Clear with the 2D engine rather than the CPU. At a period-correct
 * clock the CPU cannot clear 640x480 and still hold a frame rate; the
 * blitter can. Same rules as every other SM501 register: 32 bits at a
 * time, little-endian, so through SM501_WR.
 */
static void clear_back(void)
{
    SM501_WR(SM501_2D_DST_BASE, back);
    SM501_WR(SM501_2D_DEST, 0);
    SM501_WR(SM501_2D_DIMENSION, ((u32)SCR_W << 16) | SCR_H);
    SM501_WR(SM501_2D_PITCH, ((u32)SCR_W << 16) | SCR_W);
    SM501_WR(SM501_2D_FOREGROUND, COL_BG);
    SM501_WR(SM501_2D_STRETCH, SM501_2D_FMT_8BPP);
    SM501_WR(SM501_2D_CONTROL, SM501_2D_START | SM501_2D_CMD_RECTFILL);

    /* Wait for it: the next thing this program does is write pixels into
     * the buffer the engine is still filling. */
    while (SM501_RD(SM501_2D_STATUS) & 1) {
    }
}

static void plot(int x, int y, u8 c)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) {
        MMIO8(SM501_VRAM + back + (u32)y * SCR_W + (u32)x) = c;
    }
}

static void line(int x0, int y0, int x1, int y1, u8 c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sxp = dx < 0 ? -1 : 1;
    int syp = dy < 0 ? -1 : 1;
    int err, e2;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    err = dx - dy;

    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sxp;
        }
        if (e2 < dx) {
            err += dx;
            y0 += syp;
        }
    }
}

/* ---------------------------------------------------------------- */
/* The display                                                       */
/* ---------------------------------------------------------------- */

static void video_init(void)
{
    SM501_WR(SM501_PANEL_PALETTE + 0 * 4, 0x00000000UL);    /* black */
    SM501_WR(SM501_PANEL_PALETTE + 1 * 4, 0x0040FF40UL);    /* green */

    SM501_WR(SM501_PANEL_FB_ADDR, FB0_OFFSET);
    SM501_WR(SM501_PANEL_FB_OFFSET, SCR_W);
    SM501_WR(SM501_PANEL_FB_WIDTH, ((u32)SCR_W << 16) | SCR_W);
    SM501_WR(SM501_PANEL_FB_HEIGHT, ((u32)SCR_H << 16) | SCR_H);
    SM501_WR(SM501_PANEL_TL_LOC, 0);
    SM501_WR(SM501_PANEL_BR_LOC, ((u32)(SCR_W - 1) << 16) | (SCR_H - 1));
    SM501_WR(SM501_PANEL_H_TOTAL, SCR_W - 1);
    SM501_WR(SM501_PANEL_V_TOTAL, SCR_H - 1);
    SM501_WR(SM501_PANEL_CONTROL,
             SM501_PC_ENABLE | SM501_PC_8BPP |
             SM501_PC_FPEN | SM501_PC_VDD | SM501_PC_DATA);
}

/* Leave a blank screen rather than the last frame frozen on it, so the
 * display does not look like a machine that has hung. */
static void video_blank(void)
{
    back = FB0_OFFSET;
    clear_back();
    SM501_WR(SM501_PANEL_FB_ADDR, FB0_OFFSET);
}

/*
 * Frame pacing.
 *
 * There is no timer to wait on: the kernel has no tick yet, so there is
 * nothing to sleep against. A counted spin is what is left, and the
 * count that gives a given frame rate depends entirely on how fast the
 * machine underneath happens to be -- which, under an emulator, varies
 * by more than an order of magnitude between hosts.
 *
 * So it is measured rather than guessed. The clock only resolves to a
 * second, which is plenty: wait for a second to turn over, count spins
 * until the next one, and divide. Costs a second at start-up and is
 * right on any host.
 */
static void delay(u32 n)
{
    volatile u32 i;

    for (i = 0; i < n; i++) {
    }
}

#define CAL_CHUNK  1000

static u32 calibrate(u32 fps)
{
    time_t t0, t1;
    u32 chunks = 0;

    if (fps == 0) {
        return 0;
    }

    t0 = time(0);
    if (t0 == 0) {
        /* No clock. Fall back to a number that is wrong everywhere
         * rather than pretending to have measured something. */
        return 200000 / fps;
    }

    /* Start on a second boundary, so the count covers a whole second and
     * not the tail end of one. */
    while ((t1 = time(0)) == t0) {
    }
    t0 = t1;

    while (time(0) == t0) {
        delay(CAL_CHUNK);
        chunks++;
    }

    /* chunks * CAL_CHUNK spins take a second; a frame gets 1/fps of it.
     * The drawing itself also takes time, so the real rate comes out
     * somewhat under the target -- which is the right way to be wrong. */
    return (chunks * CAL_CHUNK) / fps;
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
    u32 pace;

    if (argc > 1) {
        fps = parse_u32(argv[1], fps);
        if (fps == 0 || fps > 1000) {
            eputs("usage: cube [frames-per-second]\n");
            return 2;
        }
    }

    puts("cube: rotating wireframe, 640x480, Q12 fixed point\n");
    puts("measuring how fast this machine is");
    pace = calibrate(fps);
    puts("... ");
    putdec(fps);
    puts(" fps wanted, ");
    putdec(pace);
    puts(" spins per frame\n");
    puts("press any key to stop\n");

    video_init();
    back = FB1_OFFSET;

    while (!key_waiting()) {
        int i, rx, ry, rz;

        clear_back();

        /* Which faces point at the viewer? A face is visible when its
         * rotated normal has positive Z. */
        for (i = 0; i < 6; i++) {
            rotate(face_n[i][0], face_n[i][1], face_n[i][2],
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
                line(sx[edge[i][0]], sy[edge[i][0]],
                     sx[edge[i][1]], sy[edge[i][1]], COL_EDGE);
            }
        }

        /* Show what was just drawn and start on the other buffer. */
        SM501_WR(SM501_PANEL_FB_ADDR, back);
        back = (back == FB0_OFFSET) ? FB1_OFFSET : FB0_OFFSET;

        ax = (ax + 1) & 255;
        ay = (ay + 2) & 255;
        az = (az + 1) & 255;
        frames++;

        delay(pace);
    }

    /* Take the keystroke that stopped it, so it does not turn up at the
     * shell prompt as a stray command. */
    {
        char c;

        read(STDIN_FILENO, &c, 1);
    }

    video_blank();

    puts("cube: ");
    putdec(frames);
    puts(" frames\n");
    return 0;
}
