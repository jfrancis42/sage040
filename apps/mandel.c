/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mandel.c - the Mandelbrot set and its Julia sets, to explore.
 *
 *     sage$ mandel [options]     (mandel -h lists them)
 *
 * For each point c of the plane, iterate z = z^2 + c from z = 0 and
 * count the steps until |z| passes 2; the points that never escape are
 * the set, black, and the rest are coloured by how long they took. A
 * Julia set is the same iteration with c fixed and z starting at the
 * point -- j switches to the Julia set of whatever point is at the
 * centre of the screen, which is how the two are related: a c inside
 * the Mandelbrot set gives a connected Julia set, one outside it dust.
 *
 * DRAWN PROGRESSIVELY: one point in every 8x8 block first, each filling
 * its block, then 4x4, 2x2 and every pixel, never working a point out
 * twice. A coarse picture is on the screen at once and sharpens, and a
 * key pressed half way starts the new view without waiting.
 *
 * FIXED POINT OR THE FPU. By default the arithmetic is 32-bit integers
 * with 28 fractional bits (the 68040 multiplies two of them to 64 bits
 * in one instruction); -f uses the FPU's doubles. Each finished picture
 * prints how long it took, so the two are a benchmark of each other --
 * and fixed point runs out of bits at a zoom of about a million, where
 * the picture turns to blocks, which is when to press f.
 *
 * Keys: the arrows pan, + and - (or z and x) zoom, j Mandelbrot/Julia,
 * f fixed point/FPU, i and I double and halve the iterations, c new
 * colours, r the starting view, q stops.
 */
#include "gfx.h"
#include "malloc.h"

#define FIRST   (GFX_PAL_FREE + 1)      /* escaped points */
#define NCOL    (256 - FIRST)
#define INSIDE  GFX_PAL_FREE            /* black          */

#define FRAC    28
#define FONE    (1L << FRAC)

enum { P_ITER, P_FPU, P_JULIA, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_ITER]  = GFX_OPT_NUM('i', "iterations", 256, 16, 65535, ""),
    [P_FPU]   = GFX_OPT_FLAG('f', "use the FPU, not fixed point"),
    [P_JULIA] = GFX_OPT_FLAG('j', "start with a Julia set"),
};

static struct gfx g;
static int w, h;
static u8 *img;                 /* the picture, a byte a pixel      */
static double cx, cy, scale;    /* centre, and units per pixel      */
static double jx, jy;           /* the Julia set's c                */
static int julia, fpu;
static u32 maxit;

static void reset_view(void)
{
    cx = julia ? 0.0 : -0.6;
    cy = 0.0;
    scale = 3.2 / (double)w;
}

static void new_colours(void)
{
    u32 keys[5], i;

    for (i = 0; i < 5; i++) {
        keys[i] = gfx_rand() & 0xffffffUL;
    }
    keys[0] = 0x000040UL;               /* dark blue first: the far field */
    gfx_ramp(&g, FIRST, NCOL, keys, 5, 1);
    gfx_colour(&g, INSIDE, 0);
}

/* Q28 multiply: the full 64-bit product, back to Q28. */
static inline s32 fmul(s32 a, s32 b)
{
    return (s32)(((s64)a * b) >> FRAC);
}

static u32 iter_fixed(s32 x, s32 y, s32 ax, s32 ay)
{
    u32 n;

    for (n = 0; n < maxit; n++) {
        s32 x2 = fmul(x, x), y2 = fmul(y, y);

        if ((u32)x2 + (u32)y2 > (u32)(4 * FONE)) {
            break;
        }
        y = 2 * fmul(x, y) + ay;
        x = x2 - y2 + ax;
    }
    return n;
}

static u32 iter_fpu(double x, double y, double ax, double ay)
{
    u32 n;

    for (n = 0; n < maxit; n++) {
        double x2 = x * x, y2 = y * y;

        if (x2 + y2 > 4.0) {
            break;
        }
        y = 2.0 * x * y + ay;
        x = x2 - y2 + ax;
    }
    return n;
}

/* The view in Q28 too, worked out once a picture, so that fixed point
 * really is fixed point: not an FPU instruction per pixel. */
static s32 fcx, fcy, fscale, fjx, fjy;

static void fixed_view(void)
{
    fcx = (s32)(cx * FONE);
    fcy = (s32)(cy * FONE);
    fscale = (s32)(scale * FONE);
    if (fscale < 1) {
        fscale = 1;                     /* out of bits: see render() */
    }
    fjx = (s32)(jx * FONE);
    fjy = (s32)(jy * FONE);
}

static u8 point(int px, int py)
{
    u32 n;

    if (fpu) {
        double x = cx + (px - w / 2) * scale, y = cy + (py - h / 2) * scale;

        n = julia ? iter_fpu(x, y, jx, jy) : iter_fpu(0, 0, x, y);
    } else {
        s32 fx = fcx + (px - w / 2) * fscale, fy = fcy + (py - h / 2) * fscale;

        n = julia ? iter_fixed(fx, fy, fjx, fjy) : iter_fixed(0, 0, fx, fy);
    }
    return n >= maxit ? INSIDE : (u8)(FIRST + n % NCOL);
}

static void show(void)
{
    u8 *draw = gfx_frame(&g);
    int y;

    for (y = 0; y < h; y++) {
        gfx_copy_row(draw + (u32)y * g.info.pitch, img + (u32)y * (u32)w,
                     (u32)w);
    }
    gfx_flip(&g);
}

/*
 * One whole picture, a pass at a time. Returns the key that cut it
 * short, or GFX_NOKEY if it finished.
 */
static int render(void)
{
    static const int steps[4] = { 8, 4, 2, 1 };
    u32 t0 = times(0);
    int s, x, y, bx, by;

    fixed_view();

    for (s = 0; s < 4; s++) {
        int st = steps[s];

        for (y = 0; y < h; y += st) {
            int k = gfx_key();

            if (k != GFX_NOKEY) {
                return k;
            }
            for (x = 0; x < w; x += st) {
                u8 c;

                /* Done in an earlier, coarser pass. */
                if (s > 0 && x % (st * 2) == 0 && y % (st * 2) == 0) {
                    continue;
                }
                c = point(x, y);
                for (by = y; by < y + st && by < h; by++) {
                    for (bx = x; bx < x + st && bx < w; bx++) {
                        img[(u32)by * (u32)w + (u32)bx] = c;
                    }
                }
            }
        }
        show();
    }
    puts("mandel: ");
    puts(julia ? "Julia" : "Mandelbrot");
    puts(", ");
    putdec(maxit);
    puts(" iterations, ");
    puts(fpu ? "FPU" : "fixed point");
    puts(": ");
    putdec((times(0) - t0) * (1000 / HZ));
    puts(" ms, width ");
    gfx_put_real(scale * w, 9);
    putch('\n');
    if (!fpu && scale * FONE < 64.0) {
        puts("mandel: fixed point is out of bits at this zoom; f for the FPU\n");
    }
    return GFX_NOKEY;
}

int main(int argc, char **argv)
{
    int k, r, again = 1;

    r = gfx_options(argc, argv, "mandel", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    maxit = par[P_ITER].value;
    fpu = (int)par[P_FPU].value;
    julia = (int)par[P_JULIA].value;
    jx = -0.8;
    jy = 0.156;

    if (gfx_open(&g, "mandel", GFX_MAP) < 0) {
        return 1;
    }
    w = (int)g.info.width;
    h = (int)g.info.height;
    img = malloc((u32)w * (u32)h);
    if (!img) {
        gfx_close(&g);
        eputs("mandel: not enough memory\n");
        return 1;
    }
    gfx_seed();
    new_colours();
    reset_view();
    puts("mandel: arrows pan, + - zoom, j Julia, f FPU, i I iterations, "
         "c colours, r reset, q stops\n");

    k = GFX_NOKEY;
    for (;;) {
        if (again) {
            again = 0;
            k = render();
        }
        if (k == GFX_NOKEY) {
            k = gfx_key_wait();
        }
        if (gfx_quit_key(k)) {
            break;
        }
        again = 1;
        switch (k) {
        case GFX_KEY_LEFT:  cx -= scale * w / 8; break;
        case GFX_KEY_RIGHT: cx += scale * w / 8; break;
        case GFX_KEY_UP:    cy -= scale * h / 8; break;
        case GFX_KEY_DOWN:  cy += scale * h / 8; break;
        case '+': case '=': case 'z': scale /= 2; break;
        case '-': case 'x':
            /* Everything of interest is within 2 of the origin, and a
             * wider view would overflow fixed point's pixel offsets. */
            if (scale * w < 8.0) scale *= 2;
            break;
        case 'j':
            if (!julia) {
                jx = cx;                /* the Julia set of this point */
                jy = cy;
            }
            julia = !julia;
            reset_view();
            break;
        case 'f': fpu = !fpu; break;
        case 'i': if (maxit < 32768) maxit *= 2; break;
        case 'I': if (maxit > 16) maxit /= 2; break;
        case 'c': new_colours(); again = 0; break;
        case 'r': reset_view(); break;
        default:  again = 0; break;
        }
        k = GFX_NOKEY;
    }
    gfx_close(&g);
    return 0;
}
