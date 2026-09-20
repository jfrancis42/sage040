/*
 * cube.c - a rotating 3D wireframe cube on the Sage040.
 *
 * A companion to c64-wireframe-cube, doing the same job on a machine two
 * decades younger.  Everything is computed live, every frame: rotation,
 * projection, hidden-line removal and line drawing from first principles.
 * Nothing is precomputed except the sine table.
 *
 * What the 68040 buys over the 6502
 * ---------------------------------
 *   - A real 32x32 multiply.  The C64 version needs the quarter-square
 *     table trick to avoid shift-and-add; here `muls.l` is one
 *     instruction, so the arithmetic is written as plain C.
 *   - Perspective projection.  The C64 README lists this as "not yet
 *     implemented" because it needs a division; the 68040 has `divs.l`,
 *     so the cube here has actual depth rather than being projected flat
 *     down the Z axis.
 *   - 256 angle steps instead of 64, for a visibly smoother tumble.
 *   - A 640x480 framebuffer instead of 320x200, double buffered in the
 *     SM501's own 16 MiB of video memory.
 *
 * What is the same
 * ----------------
 *   - Eight vertices at (+-64, +-64, +-64), rotated about all three axes
 *     each frame, with the axes advancing at different rates (+1, +2, +1)
 *     so the tumble never repeats quickly.
 *   - Hidden-line removal by convex-solid backface culling: an edge is
 *     drawn only if at least one of the two faces it joins points at the
 *     viewer.
 *   - Bresenham lines into a back buffer, then a buffer flip, so the
 *     viewer never sees a half-drawn frame.
 *
 * MC68901 timer D interrupts every 10 ms and paces the animation to a
 * fixed 50 frames per second, the way a real program would sync to a
 * timer rather than running as fast as the machine allows.
 *
 * At start-up the program measures how fast it could go with each of the
 * two clear methods.  Run it under QEMU's -icount (make run SPEED=6, the
 * default) and those numbers describe a machine of roughly 68040 speed
 * and are reproducible run to run; without it they describe the host.
 */
#include "sage040.h"

/* ---------------------------------------------------------------- */
/* Display                                                           */
/* ---------------------------------------------------------------- */
#define SCR_W       640
#define SCR_H       480
#define SCR_BYTES   (SCR_W * SCR_H)

/* Two framebuffers, 1 MiB apart in video memory. */
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

/*
 * Sign convention, which is easy to get backwards: +Z points AT the
 * viewer.  Backface culling keeps a face whose rotated normal has
 * positive Z, so a vertex with larger Z is NEARER and its distance from
 * the camera is DIST - z, not DIST + z.  Getting that wrong inverts the
 * perspective - far corners draw larger than near ones - which reads as
 * a badly distorted cube rather than an obviously broken one.
 *
 * DIST controls how strong the perspective is, as the ratio by which the
 * nearest corner draws larger than the farthest:
 *
 *      (DIST + R) / (DIST - R),  where R = sqrt(3) * HALF = 110.9
 *
 *   DIST  350 -> 1.93   severe wide angle, faces collapse to slivers
 *   DIST  700 -> 1.38
 *   DIST 1000 -> 1.25   natural, and what is used here
 *   DIST 1600 -> 1.15   nearly orthographic
 *
 * SCALE is then chosen to keep the cube about 200 pixels from centre at
 * its widest, which fits 640x480 with margin.
 */
#define CX          (SCR_W / 2)
#define CY          (SCR_H / 2)

#define FRAC        12          /* sine table fixed point: 4096 = 1  */
#define ONE         (1 << FRAC)

/*
 * Quarter-turn of sine in Q12, 64 entries: sin(i * 2pi / 256) for
 * i = 0..63.  The full circle is reconstructed by symmetry in isin().
 * Generated once and checked in, so the program needs no float support
 * and no start-up computation.
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
    if (a <= 64) {
        return sin_q[a];
    }
    if (a <= 128) {
        return sin_q[128 - a];
    }
    if (a <= 192) {
        return -sin_q[a - 128];
    }
    return -sin_q[256 - a];
}

#ifndef USE_FLOAT
#define USE_FLOAT 0
#endif

#if !USE_FLOAT
static int icos(int a)
{
    return isin(a + 64);
}
#endif

/*
 * ----------------------------------------------------------------
 * Optional floating-point variant.  Build with -DUSE_FLOAT=1 to do the
 * rotation and the perspective divide on the 68040's on-chip FPU
 * instead of in Q12 fixed point.  Everything else - the sine table, the
 * screen coordinates, the framebuffer - stays integer, because that is
 * what the hardware wants either way.
 *
 * This exists to exercise the FPU with real work and to compare the two
 * approaches honestly.  See the README for what the comparison does and
 * does not tell you.
 * ----------------------------------------------------------------
 */
#if USE_FLOAT

typedef float real;

static real sin_real[256];

/* Built once from the Q12 table, so both variants use the same angles. */
static void build_sin_real(void)
{
    int i;

    for (i = 0; i < 256; i++) {
        sin_real[i] = (real)isin(i) * (real)(1.0 / 4096.0);
    }
}

static real rsin(int a) { return sin_real[a & 255]; }
static real rcos(int a) { return sin_real[(a + 64) & 255]; }

#endif /* USE_FLOAT */

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

/* Twelve edges: two vertices, then the two faces the edge joins. */
static const unsigned char edge[12][4] = {
    { 0, 1, 3, 5 }, { 2, 3, 2, 5 }, { 4, 5, 3, 4 }, { 6, 7, 2, 4 },
    { 0, 2, 1, 5 }, { 1, 3, 0, 5 }, { 4, 6, 1, 4 }, { 5, 7, 0, 4 },
    { 0, 4, 1, 3 }, { 1, 5, 0, 3 }, { 2, 6, 1, 2 }, { 3, 7, 0, 2 },
};

static int sx[8], sy[8];        /* projected screen coordinates */
static int visible[6];          /* per-face: does it face the viewer? */

/* ---------------------------------------------------------------- */
/* Rotation                                                          */
/* ---------------------------------------------------------------- */

/*
 * Rotate (x, y, z) about all three axes.  Each stage is the usual pair
 * of products; on the 68040 these compile to `muls.l`, which is why the
 * C64's quarter-square table is not needed here.
 */
#if USE_FLOAT

static void rotate(int xi, int yi, int zi, int ax, int ay, int az,
                   int *ox, int *oy, int *oz)
{
    real x = (real)xi, y = (real)yi, z = (real)zi;
    real s, c, t;

    s = rsin(ax); c = rcos(ax);
    t = y * c - z * s;
    z = y * s + z * c;
    y = t;

    s = rsin(ay); c = rcos(ay);
    t = x * c + z * s;
    z = z * c - x * s;
    x = t;

    s = rsin(az); c = rcos(az);
    t = x * c - y * s;
    y = x * s + y * c;
    x = t;

    *ox = (int)x; *oy = (int)y; *oz = (int)z;
}

#else

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

#endif /* USE_FLOAT */

/* ---------------------------------------------------------------- */
/* Drawing                                                           */
/* ---------------------------------------------------------------- */

static u32 back;                /* video-memory offset of the back buffer */

/*
 * Clearing the back buffer is by far the most expensive thing a frame
 * does: 307,200 bytes against eight vertices and at most nine short
 * lines.  There are two ways to do it.
 */
static int use_2d = 1;          /* 0 = CPU stores, 1 = SM501 2D engine */

/* 76,800 long writes by the CPU. */
static void clear_back_cpu(void)
{
    u32 *p = (u32 *)(SM501_VRAM + back);
    int n = SCR_BYTES / 4;

    while (n--) {
        *p++ = 0;
    }
}

/*
 * The same clear as a single Rectangle Fill in the SM501's 2D engine:
 * seven register writes, and the display controller does the work.
 */
static void clear_back_2d(void)
{
    SM501_WR(SM501_2D_DST_BASE, back);
    SM501_WR(SM501_2D_DEST, 0);                             /* x = y = 0 */
    SM501_WR(SM501_2D_DIMENSION, ((u32)SCR_W << 16) | SCR_H);
    SM501_WR(SM501_2D_PITCH, ((u32)SCR_W << 16) | SCR_W);
    SM501_WR(SM501_2D_FOREGROUND, COL_BG);
    SM501_WR(SM501_2D_STRETCH, SM501_2D_FMT_8BPP);          /* XY addressing */
    SM501_WR(SM501_2D_CONTROL, SM501_2D_START | SM501_2D_CMD_RECTFILL);
}

static void clear_back(void)
{
    if (use_2d) {
        clear_back_2d();
    } else {
        clear_back_cpu();
    }
}

static void plot(int x, int y, u8 c)
{
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) {
        MMIO8(SM501_VRAM + back + (u32)y * SCR_W + (u32)x) = c;
    }
}

/* Bresenham, with the pixel test inlined by plot(). */
static void line(int x0, int y0, int x1, int y1, u8 c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int ax, ay, sxs, sys, err;

    ax = dx < 0 ? -dx : dx;
    ay = dy < 0 ? -dy : dy;
    sxs = dx < 0 ? -1 : 1;
    sys = dy < 0 ? -1 : 1;

    if (ax == 0 && ay == 0) {
        return;                 /* degenerate: skip, as the C64 does */
    }

    if (ax >= ay) {
        err = ax >> 1;
        for (;;) {
            plot(x0, y0, c);
            if (x0 == x1) {
                break;
            }
            err -= ay;
            if (err < 0) {
                y0 += sys;
                err += ax;
            }
            x0 += sxs;
        }
    } else {
        err = ay >> 1;
        for (;;) {
            plot(x0, y0, c);
            if (y0 == y1) {
                break;
            }
            err -= ax;
            if (err < 0) {
                x0 += sxs;
                err += ay;
            }
            y0 += sys;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Hardware setup                                                    */
/* ---------------------------------------------------------------- */

static void video_init(void)
{
    /* Palette: 0 is black, 1 is the phosphor green the C64 version wore. */
    SM501_WR(SM501_PANEL_PALETTE + 0 * 4, 0x00000000UL);
    SM501_WR(SM501_PANEL_PALETTE + 1 * 4, 0x0040FF40UL);

    SM501_WR(SM501_PANEL_FB_ADDR, FB0_OFFSET);
    SM501_WR(SM501_PANEL_FB_OFFSET, SCR_W);
    SM501_WR(SM501_PANEL_FB_WIDTH, ((u32)SCR_W << 16) | SCR_W);
    SM501_WR(SM501_PANEL_FB_HEIGHT, ((u32)SCR_H << 16) | SCR_H);
    SM501_WR(SM501_PANEL_TL_LOC, 0);
    SM501_WR(SM501_PANEL_BR_LOC, ((u32)(SCR_W - 1) << 16) | (SCR_H - 1));

    /* Width and height come from the totals; the model uses total+1. */
    SM501_WR(SM501_PANEL_H_TOTAL, SCR_W - 1);
    SM501_WR(SM501_PANEL_V_TOTAL, SCR_H - 1);

    SM501_WR(SM501_PANEL_CONTROL,
             SM501_PC_ENABLE | SM501_PC_8BPP |
             SM501_PC_FPEN | SM501_PC_VDD | SM501_PC_DATA);
}

/*
 * Timer D interrupts every 10 ms and the shared MFP handler counts them,
 * which gives a clock to measure frame rate against.
 * 200 prescale x 123 = 24600 cycles / 2.4576 MHz = 10.0 ms.
 */
#define TICK_MS     10
#define FRAME_TICKS 2           /* 2 x 10 ms => 50 frames per second */

static void timer_init(void)
{
    mfp_install_vectors(0x40);
    MMIO8(MFP_VR) = 0x40;               /* automatic end-of-interrupt */
    mfp_enable(MFPCH_TIMERD);
    mfp_clear_pending(MFPCH_TIMERD);
    MMIO8(MFP_TDDR) = 123;
    MMIO8(MFP_TCDCR) = (u8)((MMIO8(MFP_TCDCR) & 0x70) | MFP_TC_DIV200);
    mfp_set_ipl(0);                     /* let interrupts in */
}

static u32 ticks(void)
{
    return (u32)mfp_vec_count[MFPCH_TIMERD];
}

/* ---------------------------------------------------------------- */
/* One frame: rotate, cull, project, draw.  No flip, no pacing.      */
/* ---------------------------------------------------------------- */

static void render(int ax, int ay, int az)
{
    int i, rx, ry, rz;

    clear_back();

    /* Which faces point at the viewer? */
    for (i = 0; i < 6; i++) {
        rotate(face_n[i][0] * ONE, face_n[i][1] * ONE, face_n[i][2] * ONE,
               ax, ay, az, &rx, &ry, &rz);
        visible[i] = rz > 0;
    }

    /* Rotate and project the eight vertices. */
    for (i = 0; i < 8; i++) {
        int d;

        rotate(vtx[i][0] * HALF, vtx[i][1] * HALF, vtx[i][2] * HALF,
               ax, ay, az, &rx, &ry, &rz);

        d = DIST - rz;              /* +Z is toward the viewer, so subtract */
#if USE_FLOAT
        sx[i] = CX + (int)((real)rx * (real)SCALE / (real)d);
        sy[i] = CY - (int)((real)ry * (real)SCALE / (real)d);
#else
        sx[i] = CX + (rx * SCALE) / d;
        sy[i] = CY - (ry * SCALE) / d;
#endif
    }

    /* Draw an edge only if one of its two faces is toward the viewer. */
    for (i = 0; i < 12; i++) {
        if (visible[edge[i][2]] || visible[edge[i][3]]) {
            line(sx[edge[i][0]], sy[edge[i][0]],
                 sx[edge[i][1]], sy[edge[i][1]], COL_EDGE);
        }
    }
}

/*
 * How fast can the machine draw if nothing holds it back?  Render a run
 * of frames with no pacing and no flipping and time it.
 *
 * Note this is the speed of the emulated machine as QEMU runs it, not a
 * claim about what real 68040 silicon would do - QEMU's timers follow
 * host wall-clock time, and the host is far faster than a 25 MHz 68040.
 */
#define BENCH_TICKS 50          /* 50 x 10 ms = half a second */

static u32 benchmark(int with_2d)
{
    u32 t, n = 0;
    int save = use_2d;

    use_2d = with_2d;
    t = ticks();
    while (ticks() - t < 1) {
        /* start on a tick boundary */
    }
    t = ticks();
    while (ticks() - t < BENCH_TICKS) {
        render((int)(n & 255), (int)((n * 2) & 255), (int)(n & 255));
        n++;
    }
    use_2d = save;
    return (n * 1000UL) / (BENCH_TICKS * TICK_MS);
}

/*
 * Print the projected screen coordinates for one fixed orientation.
 * Both arithmetic variants print this, so the two can be diffed to see
 * exactly where fixed point and floating point disagree.
 */
static void geometry_check(void)
{
    int i;

    render(30, 45, 0);
    uart_puts("geometry at ax=30 ay=45 az=0:\n");
    for (i = 0; i < 8; i++) {
        uart_puts("  v"); uart_putdec((u32)i);
        uart_puts(" ("); uart_putdec((u32)sx[i]);
        uart_puts(","); uart_putdec((u32)sy[i]);
        uart_puts(")\n");
    }
    uart_putc('\n');
}

int main(void)
{
    int ax = 0, ay = 0, az = 0;
    u32 frames = 0;
    u32 t0, next, fast;

    uart_init();
#if USE_FLOAT
    build_sin_real();
#endif
    uart_puts("\nSage040 wireframe cube\n");
    uart_puts("640x480, 8bpp, double buffered in SM501 video memory\n");
    uart_puts("8 vertices, 12 edges, backface culled, perspective projected\n");
#if USE_FLOAT
    uart_puts("arithmetic: 68040 on-chip FPU, single precision\n\n");
#else
    uart_puts("arithmetic: Q12 fixed point, integer unit only\n\n");
#endif

    video_init();
    timer_init();
    back = FB1_OFFSET;

    {
        u32 cpu_fps = benchmark(0);
        u32 gpu_fps = benchmark(1);

        uart_puts("unthrottled frame rate (see SPEED= in the Makefile):\n");
        uart_puts("  clear by CPU stores    "); uart_putdec(cpu_fps);
        uart_puts(" frames/sec\n");
        uart_puts("  clear by 2D engine     "); uart_putdec(gpu_fps);
        uart_puts(" frames/sec");
        if (cpu_fps) {
            uart_puts("   ("); uart_putdec(gpu_fps * 10 / cpu_fps);
            uart_puts("/10 x faster)");
        }
        uart_puts("\n\n");
        fast = gpu_fps;
    }
    (void)fast;
    uart_puts("pacing to 50 frames/sec\n\n");

    geometry_check();

    t0 = ticks();
    next = t0;

    for (;;) {
        render(ax, ay, az);

        /* Show the buffer just drawn, and draw into the other one next. */
        SM501_WR(SM501_PANEL_FB_ADDR, back);
        back = (back == FB0_OFFSET) ? FB1_OFFSET : FB0_OFFSET;

        /* Pace to a fixed frame rate instead of running flat out. */
        next += FRAME_TICKS;
        while ((s32)(ticks() - next) < 0) {
            /* idle until the frame is due */
        }

        ax = (ax + 1) & 255;
        ay = (ay + 2) & 255;
        az = (az + 1) & 255;
        frames++;

        if ((frames % 250) == 0) {
            u32 dt = ticks() - t0;              /* ticks for 250 frames */
            u32 fps10 = dt ? 250000UL / dt : 0; /* frames/sec x 10      */

            uart_puts("frame "); uart_putdec(frames);
            uart_puts("  fps ");
            uart_putdec(fps10 / 10);
            uart_putc('.');
            uart_putdec(fps10 % 10);
            uart_putc('\n');
            t0 = ticks();
        }
    }
}
