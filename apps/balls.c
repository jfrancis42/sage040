/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * balls.c - balls bouncing off the walls and each other.
 *
 *     sage$ balls [options]      (balls -h lists them)
 *
 * Every ball has a position, a velocity and a size, in 24.8 fixed
 * point. Gravity pulls them down; the walls reflect them, losing a
 * little each bounce; and two balls that overlap collide elastically --
 * their velocities along the line between their centres are exchanged
 * in proportion to their masses (their areas), the part across that
 * line is kept, and they are pushed apart so they do not stick. Energy
 * and momentum are what a real collision keeps, so a crowd of them
 * behaves like one.
 *
 * Keys: g turns gravity off and on, space pauses, r throws them again,
 * q stops.
 */
#include "gfx.h"
#include "malloc.h"

#define FP      8                       /* 24.8 */

enum { P_BALLS, P_GRAVITY, P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_BALLS]   = GFX_OPT_NUM('n', "balls", 24, 1, 200, ""),
    [P_GRAVITY] = GFX_OPT_NUM('G', "gravity", 1, 0, 1, "0 or 1"),
    [P_FPS]     = GFX_OPT_NUM('F', "frames per second", 30, 0, 200,
                              "0 for as fast as it will go"),
};

struct ball {
    s32 x, y, vx, vy;           /* 24.8 */
    int r;                      /* pixels */
    u8 ink;
};

static struct gfx g;
static struct ball *balls;
static u32 nballs;
static int w, h;

static void throw_all(void)
{
    u32 i;

    for (i = 0; i < nballs; i++) {
        struct ball *b = &balls[i];

        b->r = 6 + (int)(gfx_rand() % 22);
        b->x = (s32)(b->r + (int)(gfx_rand() % (u32)(w - 2 * b->r))) << FP;
        b->y = (s32)(b->r + (int)(gfx_rand() % (u32)(h / 2))) << FP;
        b->vx = (s32)(gfx_rand() % 2048) - 1024;
        b->vy = (s32)(gfx_rand() % 1024) - 512;
        b->ink = (u8)(GFX_PAL_FREE + 1 + i % 16);
    }
}

static void collide(struct ball *a, struct ball *b)
{
    s32 dx = (b->x - a->x) >> FP, dy = (b->y - a->y) >> FP;
    int rr = a->r + b->r;
    s32 d2 = dx * dx + dy * dy, d, nx, ny, va, vb, ma, mb, p;

    if (d2 >= rr * rr || d2 == 0) {
        return;
    }
    d = (s32)gfx_isqrt((u32)d2);
    if (d == 0) {
        d = 1;
    }
    nx = (dx << FP) / d;                /* the unit normal, 24.8 */
    ny = (dy << FP) / d;
    va = (a->vx * nx + a->vy * ny) >> FP;   /* speeds along it */
    vb = (b->vx * nx + b->vy * ny) >> FP;
    if (va - vb <= 0) {
        goto separate;                  /* already moving apart */
    }
    ma = a->r * a->r;
    mb = b->r * b->r;
    /* The impulse that exchanges momentum elastically. */
    p = 2 * (va - vb) * mb / (ma + mb);
    a->vx -= (p * nx) >> FP;
    a->vy -= (p * ny) >> FP;
    p = 2 * (va - vb) * ma / (ma + mb);
    b->vx += (p * nx) >> FP;
    b->vy += (p * ny) >> FP;
separate:
    /* Out of each other's way, half the overlap each. */
    p = (rr - d + 1) << (FP - 1);
    a->x -= (p * nx) >> FP;
    a->y -= (p * ny) >> FP;
    b->x += (p * nx) >> FP;
    b->y += (p * ny) >> FP;
}

static void move(void)
{
    u32 i, j;

    for (i = 0; i < nballs; i++) {
        struct ball *b = &balls[i];
        s32 lo, hi;

        if (par[P_GRAVITY].value) {
            b->vy += 24;
        }
        b->x += b->vx;
        b->y += b->vy;
        lo = (s32)b->r << FP;
        hi = (s32)(w - b->r) << FP;
        if (b->x < lo) { b->x = lo; b->vx = -b->vx * 15 / 16; }
        if (b->x > hi) { b->x = hi; b->vx = -b->vx * 15 / 16; }
        hi = (s32)(h - b->r) << FP;
        if (b->y < lo) { b->y = lo; b->vy = -b->vy * 15 / 16; }
        if (b->y > hi) { b->y = hi; b->vy = -b->vy * 15 / 16; }
    }
    for (i = 0; i < nballs; i++) {
        for (j = i + 1; j < nballs; j++) {
            collide(&balls[i], &balls[j]);
        }
    }
}

static void disc(u8 *draw, int cx, int cy, int r, u8 ink)
{
    int y;

    for (y = -r; y <= r; y++) {
        int half = (int)gfx_isqrt((u32)(r * r - y * y));
        int x0 = cx - half, x1 = cx + half;

        if (cy + y < 0 || cy + y >= h) {
            continue;
        }
        if (x0 < 0) x0 = 0;
        if (x1 >= w) x1 = w - 1;
        if (x1 >= x0) {
            gfx_fill_row(draw + (u32)(cy + y) * g.info.pitch + (u32)x0, ink,
                         (u32)(x1 - x0 + 1));
        }
    }
}

int main(int argc, char **argv)
{
    static const u32 keys[4] = {
        0xff4040UL, 0xffe040UL, 0x40ff80UL, 0x4080ffUL
    };
    struct gfx_clock clk;
    u32 i;
    int k, r;

    r = gfx_options(argc, argv, "balls", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    nballs = par[P_BALLS].value;
    if (gfx_open(&g, "balls", GFX_MAP) < 0) {
        return 1;
    }
    balls = malloc(nballs * sizeof(*balls));
    if (!balls) {
        gfx_close(&g);
        eputs("balls: not enough memory\n");
        return 1;
    }
    w = (int)g.info.width;
    h = (int)g.info.height;
    gfx_seed();
    gfx_colour(&g, GFX_PAL_FREE, 0x101018UL);
    gfx_ramp(&g, GFX_PAL_FREE + 1, 16, keys, 4, 1);
    throw_all();
    puts("balls: g gravity, r throws again, space pauses, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        u8 *draw;

        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case 'g': par[P_GRAVITY].value = !par[P_GRAVITY].value; break;
        case 'r': throw_all(); break;
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
        move();
        gfx_clear(&g, GFX_PAL_FREE);
        ioctl(g.fd, FBIO_SYNC, 0);
        draw = gfx_frame(&g);
        for (i = 0; i < nballs; i++) {
            disc(draw, balls[i].x >> FP, balls[i].y >> FP, balls[i].r,
                 balls[i].ink);
        }
        gfx_flip(&g);
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);
    gfx_clock_summary(&clk, "balls", "frames");
    putch('\n');
    return 0;
}
