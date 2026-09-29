/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * gfx.h - what a graphics program on /dev/fb0 needs, once.
 *
 * cube, boids, wator and life each carried their own copies of the same
 * pieces -- the frame pacing, the option table and its -h listing, the
 * mapping of video memory, the random numbers, the palette ramps -- and
 * four copies are where they start to drift: the frame-timing fix had
 * to be made in every one of them. They live here now, and a new demo
 * is mostly its own idea.
 *
 * Built into every program alongside ulib, and --gc-sections drops
 * whatever a program does not call, so a program that never draws pays
 * nothing for this.
 *
 * Nothing here touches hardware. The screen is /dev/fb0 through its
 * ioctls and its mapping; the keys come from standard input.
 */
#ifndef GFX_H
#define GFX_H

#include "ulib.h"

/* ---------------------------------------------------------------- */
/* Options                                                           */
/* ---------------------------------------------------------------- */

enum { GFX_NUM, GFX_FLAG, GFX_STR };

/*
 * One command-line option. A program keeps an array of these indexed
 * by its own enum, and reads `value` (or `str`) after gfx_options().
 * -x (the random seed) and -h are every program's, and are not listed.
 */
struct gfx_opt {
    char opt;
    unsigned char kind;         /* GFX_NUM, GFX_FLAG or GFX_STR       */
    const char *name;
    u32 value;                  /* NUM: the number; FLAG: 0 or 1      */
    u32 min, max;               /* NUM: the range allowed             */
    const char *unit;           /* NUM: shown after the name, or ""   */
    const char *str;            /* STR: the value, and its default    */
};

#define GFX_OPT_NUM(c, name, def, lo, hi, unit) \
    { (c), GFX_NUM, (name), (def), (lo), (hi), (unit), 0 }
#define GFX_OPT_FLAG(c, name) \
    { (c), GFX_FLAG, (name), 0, 0, 1, "", 0 }
#define GFX_OPT_STR(c, name, def) \
    { (c), GFX_STR, (name), 0, 0, 0, "", (def) }

/*
 * Parse argv against the table. -N 80 and -N80 both work; flags cannot
 * be bundled. Returns 0 to go on, 1 if -h printed the listing (exit 0),
 * or -1 after saying what was wrong (exit 2). `more`, if given, is
 * called at the end of the -h listing for anything a table cannot say.
 */
int  gfx_options(int argc, char **argv, const char *prog,
                 struct gfx_opt *opts, int n, void (*more)(void));

/*
 * A decimal number -- "-1.4", "2.5e-3" -- from a GFX_STR option, or -1
 * if it is not one. And one printed with `places` digits after the
 * point, for a banner that says how to repeat a run exactly. These use
 * the FPU; a program that never calls them never carries them.
 */
int  gfx_real(const char *s, double *out);
void gfx_put_real(double v, int places);

/* ---------------------------------------------------------------- */
/* Random numbers                                                    */
/* ---------------------------------------------------------------- */

/* Seed from -x, or from the clock if -x was not given; returns the seed
 * used, so a run worth seeing again can be repeated. */
u32  gfx_seed(void);
u32  gfx_rand(void);                        /* xorshift32 */

/* ---------------------------------------------------------------- */
/* Integer maths                                                     */
/* ---------------------------------------------------------------- */

#define GFX_FRAC    12                      /* the sine's fixed point */
#define GFX_ONE     (1 << GFX_FRAC)

int  gfx_sin(int a);            /* a in 256ths of a turn, Q12 result  */
int  gfx_cos(int a);
u32  gfx_isqrt(u32 v);

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

struct gfx {
    int fd;
    struct fb_info info;        /* current as of the last gfx_frame() */
    u8 *map;                    /* all of video memory, if mapped     */
    u32 map_len;
    const char *prog;
};

#define GFX_MAP     1           /* map video memory to draw in bytes  */

/*
 * Open /dev/fb0, 8 bits a pixel, double buffered, and mapped if asked.
 * Says why on failure, prefixed with the program's name, and returns
 * -1. Also arranges that ctrl-C, ctrl-Z and the rest leave the screen
 * and the terminal as they found them.
 */
int  gfx_open(struct gfx *g, const char *prog, int flags);

/* The buffer to draw this frame in, when mapped. It changes at every
 * flip. Also re-asserts double buffering, which a write to the text
 * console on this screen turns off. */
u8  *gfx_frame(struct gfx *g);

void gfx_flip(struct gfx *g);
void gfx_clear(struct gfx *g, u32 colour);
void gfx_line(struct gfx *g, int x0, int y0, int x1, int y1, u32 colour);

/* Blank the screen, put the terminal back, unmap, close. */
void gfx_close(struct gfx *g);

/* A row of pixels into a mapped buffer, a longword at a time where it
 * can be -- ulib's memcpy goes a byte at a time, and this is most of
 * what a mapped frame costs. */
void gfx_copy_row(u8 *dst, const u8 *src, u32 n);
void gfx_fill_row(u8 *dst, u8 colour, u32 n);

/* ---------------------------------------------------------------- */
/* Colour                                                            */
/* ---------------------------------------------------------------- */

/*
 * 0-7 are the driver's colours and 16-31 the text console's; a program
 * that wants its own starts at GFX_PAL_FREE and has the rest.
 */
#define GFX_PAL_FREE    32

void gfx_colour(struct gfx *g, u32 index, u32 rgb);

/* a..b, num/den of the way: 0x00RRGGBB, channel by channel. */
u32  gfx_blend(u32 a, u32 b, u32 num, u32 den);

/*
 * `n` palette entries from `first`, interpolated through `nkeys` key
 * colours. Straight, the first entry is the first key and the last
 * the last; cyclic, the ramp runs on from the last key back round to
 * the first, for anything like a hue wheel whose ends meet.
 */
void gfx_ramp(struct gfx *g, u32 first, u32 n,
              const u32 *keys, u32 nkeys, int cyclic);

/* ---------------------------------------------------------------- */
/* Keys                                                              */
/* ---------------------------------------------------------------- */

/*
 * Keys beyond ASCII, decoded from the VT100 sequences both the serial
 * line and the PC keyboard's driver send.
 */
#define GFX_NOKEY       (-1)
#define GFX_KEY_ESC     27
#define GFX_KEY_UP      0x101
#define GFX_KEY_DOWN    0x102
#define GFX_KEY_RIGHT   0x103
#define GFX_KEY_LEFT    0x104
#define GFX_KEY_HOME    0x105
#define GFX_KEY_END     0x106
#define GFX_KEY_PGUP    0x107
#define GFX_KEY_PGDN    0x108
#define GFX_KEY_DEL     0x109

/*
 * Keys a character at a time with no echo, until gfx_close() -- or
 * until the program is killed or stopped, when the terminal is put back
 * anyway. gfx_open() does this; a program that only wants keys can call
 * it by itself. Harmless if standard input is not a terminal.
 */
void gfx_keys_raw(void);
void gfx_keys_restore(void);

/* The next key, or GFX_NOKEY if none is waiting. Never blocks. */
int  gfx_key(void);

/* The next key, waiting for one. */
int  gfx_key_wait(void);

/* q, Q or Escape: what every program takes as "stop". */
int  gfx_quit_key(int k);

/* ---------------------------------------------------------------- */
/* Frame pacing                                                      */
/* ---------------------------------------------------------------- */

/*
 * A frame schedule. Each tick sleeps until the next frame is due,
 * counted from the start rather than a period after the last, so that
 * one frame's rounding to the kernel's tick is made up by the next and
 * the average is what was asked for. A program more than a frame behind
 * starts the schedule again rather than racing to catch up, which would
 * show as a burst of speed. fps 0 is "as fast as it will go".
 */
struct gfx_clock {
    u32 fps;
    u32 start, n;               /* the schedule                       */
    u32 started, frames;        /* for the summary                    */
};

void gfx_clock_start(struct gfx_clock *c, u32 fps);
void gfx_clock_tick(struct gfx_clock *c);

/* Pausing: stop counting at pause, and at resume take up the schedule
 * again from now, with the paused time out of the summary's rate. */
u32  gfx_clock_pause(struct gfx_clock *c);
void gfx_clock_resume(struct gfx_clock *c, u32 paused_at);

/* "PROG: N NOUN in S seconds (R per second)", no newline, so that a
 * program can add its own and end the line. */
void gfx_clock_summary(const struct gfx_clock *c, const char *prog,
                       const char *noun);

#endif /* GFX_H */
