/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * stars.c - a starfield, flying through it.
 *
 *     sage$ stars [options]      (stars -h lists them)
 *
 * Every star is a point in a box in front of the viewer. Each frame it
 * comes a little closer, and it is drawn where perspective puts it: its
 * x and y divided by its distance, so near stars rush outwards and far
 * ones barely move. Brighter and bigger as it nears; one that passes
 * the viewer, or leaves the screen, goes back to the far end.
 *
 * Keys: the arrows steer, + and - change the speed, space pauses, q
 * stops.
 */
#include "gfx.h"
#include "malloc.h"

#define DEPTH   4096
#define SPAN    4096                    /* x and y within +-SPAN/2 */
#define GREYS   16

enum { P_STARS, P_SPEED, P_FPS, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_STARS] = GFX_OPT_NUM('n', "stars", 600, 1, 20000, ""),
    [P_SPEED] = GFX_OPT_NUM('s', "speed", 40, 1, 400, "units a frame"),
    [P_FPS]   = GFX_OPT_NUM('F', "frames per second", 30, 0, 200,
                            "0 for as fast as it will go"),
};

struct star {
    int x, y, z;
};

static struct gfx g;
static struct star *sky;
static int drift_x, drift_y;

static void place(struct star *s, int far)
{
    s->x = (int)(gfx_rand() % SPAN) - SPAN / 2;
    s->y = (int)(gfx_rand() % SPAN) - SPAN / 2;
    s->z = far ? DEPTH : 1 + (int)(gfx_rand() % DEPTH);
}

int main(int argc, char **argv)
{
    struct gfx_clock clk;
    u32 i, n;
    int k, r, cx, cy, w, h;

    r = gfx_options(argc, argv, "stars", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    n = par[P_STARS].value;
    if (gfx_open(&g, "stars", GFX_MAP) < 0) {
        return 1;
    }
    sky = malloc(n * sizeof(*sky));
    if (!sky) {
        gfx_close(&g);
        eputs("stars: not enough memory\n");
        return 1;
    }
    gfx_seed();
    gfx_colour(&g, GFX_PAL_FREE, 0);
    for (i = 1; i <= GREYS; i++) {
        u32 v = i * 255 / GREYS;

        gfx_colour(&g, GFX_PAL_FREE + i, (v << 16) | (v << 8) | v);
    }
    for (i = 0; i < n; i++) {
        place(&sky[i], 0);
    }
    w = (int)g.info.width;
    h = (int)g.info.height;
    cx = w / 2;
    cy = h / 2;
    puts("stars: arrows steer, + - speed, space pauses, q stops\n");

    gfx_clock_start(&clk, par[P_FPS].value);
    for (;;) {
        u8 *draw;

        k = gfx_key();
        if (gfx_quit_key(k)) {
            break;
        }
        switch (k) {
        case GFX_KEY_LEFT:  drift_x += 4; break;
        case GFX_KEY_RIGHT: drift_x -= 4; break;
        case GFX_KEY_UP:    drift_y += 4; break;
        case GFX_KEY_DOWN:  drift_y -= 4; break;
        case '+': case '=':
            if (par[P_SPEED].value < 400) par[P_SPEED].value += 10;
            break;
        case '-':
            if (par[P_SPEED].value > 10) par[P_SPEED].value -= 10;
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
        gfx_clear(&g, GFX_PAL_FREE);            /* the blitter clears */
        ioctl(g.fd, FBIO_SYNC, 0);
        draw = gfx_frame(&g);
        for (i = 0; i < n; i++) {
            struct star *s = &sky[i];
            int sx, sy, b;

            s->z -= (int)par[P_SPEED].value;
            s->x += drift_x;
            s->y += drift_y;
            if (s->z < 1) {
                place(s, 1);
                continue;
            }
            sx = cx + s->x * 256 / s->z;
            sy = cy + s->y * 256 / s->z;
            if (sx < 0 || sy < 0 || sx >= w - 1 || sy >= h - 1) {
                place(s, 1);
                continue;
            }
            b = GREYS - s->z * GREYS / (DEPTH + 1);
            draw[(u32)sy * g.info.pitch + (u32)sx] = (u8)(GFX_PAL_FREE + b);
            if (s->z < DEPTH / 4) {             /* near: two by two */
                draw[(u32)sy * g.info.pitch + (u32)sx + 1] =
                    (u8)(GFX_PAL_FREE + b);
                draw[(u32)(sy + 1) * g.info.pitch + (u32)sx] =
                    (u8)(GFX_PAL_FREE + b);
                draw[(u32)(sy + 1) * g.info.pitch + (u32)sx + 1] =
                    (u8)(GFX_PAL_FREE + b);
            }
        }
        gfx_flip(&g);
        gfx_clock_tick(&clk);
    }
done:
    gfx_close(&g);
    gfx_clock_summary(&clk, "stars", "frames");
    putch('\n');
    return 0;
}
