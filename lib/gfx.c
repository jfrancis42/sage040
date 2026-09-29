/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * gfx.c - see gfx.h.
 */
#include "gfx.h"

/* ---------------------------------------------------------------- */
/* Options                                                           */
/* ---------------------------------------------------------------- */

static u32 opt_seed;            /* -x; 0 means "from the clock"      */

static void usage(const char *prog, const struct gfx_opt *opts, int n,
                  void (*more)(void))
{
    int i;

    puts("usage: ");
    puts(prog);
    puts(" [options]\n");
    for (i = 0; i < n; i++) {
        const struct gfx_opt *o = &opts[i];
        char buf[4] = { ' ', '-', o->opt, '\0' };

        puts(buf);
        puts("  ");
        puts(o->name);
        if (o->kind == GFX_NUM) {
            if (o->unit[0]) {
                puts(", ");
                puts(o->unit);
            }
            puts(" (default ");
            putdec(o->value);
            putch(')');
        } else if (o->kind == GFX_STR && o->str) {
            puts(" (default ");
            puts(o->str);
            putch(')');
        }
        putch('\n');
    }
    puts(" -x  random seed (default: from the clock)\n");
    puts(" -h  this\n");
    if (more) {
        more();
    }
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

static void complain(const char *prog, const char *a, const char *b,
                     const char *c)
{
    eputs(prog);
    eputs(": ");
    eputs(a);
    eputs(b);
    eputs(c);
    eputs("\n");
}

int gfx_options(int argc, char **argv, const char *prog,
                struct gfx_opt *opts, int n, void (*more)(void))
{
    int i, k;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val;
        struct gfx_opt *o = 0;
        u32 v;

        if (a[0] != '-' || a[1] == '\0') {
            complain(prog, "not an option: ", a, "");
            goto bad;
        }
        if (a[1] == 'h' && a[2] == '\0') {
            usage(prog, opts, n, more);
            return 1;
        }
        for (k = 0; k < n; k++) {
            if (opts[k].opt == a[1]) {
                o = &opts[k];
                break;
            }
        }
        if (!o && a[1] != 'x') {
            complain(prog, "unknown option ", a, "");
            goto bad;
        }
        if (o && o->kind == GFX_FLAG) {
            if (a[2]) {
                complain(prog, "-", a + 1, " takes no value");
                goto bad;
            }
            o->value = 1;
            continue;
        }

        val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
        if (!val) {
            complain(prog, "-", a + 1, " wants a value");
            goto bad;
        }
        if (o && o->kind == GFX_STR) {
            o->str = val;
            continue;
        }
        if (parse_u32(val, &v) < 0) {
            complain(prog, "-", a + 1, " wants a number");
            goto bad;
        }
        if (!o) {
            opt_seed = v;
            continue;
        }
        if (v < o->min || v > o->max) {
            complain(prog, o->name, " out of range", "");
            goto bad;
        }
        o->value = v;
    }
    return 0;

bad:
    eputs(prog);
    eputs(" -h lists the options\n");
    return -1;
}

/* ---------------------------------------------------------------- */
/* Random numbers                                                    */
/* ---------------------------------------------------------------- */

static u32 rng_state = 1;

u32 gfx_seed(void)
{
    u32 s = opt_seed;

    if (s == 0) {
        s = (u32)time(0) ^ times(0);
    }
    rng_state = s ? s : 1;          /* xorshift is stuck at 0 */
    return s;
}

u32 gfx_rand(void)
{
    u32 x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* ---------------------------------------------------------------- */
/* Integer maths                                                     */
/* ---------------------------------------------------------------- */

/*
 * Quarter-turn of sine in Q12: sin(i * 2pi / 256) for i = 0..64. The
 * rest of the circle comes from symmetry. Checked in rather than
 * computed, so a program needs no floating point to turn things round.
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

int gfx_sin(int a)
{
    a &= 255;
    if (a <= 64)  return sin_q[a];
    if (a <= 128) return sin_q[128 - a];
    if (a <= 192) return -sin_q[a - 128];
    return -sin_q[256 - a];
}

int gfx_cos(int a)
{
    return gfx_sin(a + 64);
}

u32 gfx_isqrt(u32 v)
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

/* ---------------------------------------------------------------- */
/* Keys                                                              */
/* ---------------------------------------------------------------- */

static struct termios saved_tio;
static int raw_on;

void gfx_keys_raw(void)
{
    struct termios t;

    if (raw_on || tcgetattr(STDIN_FILENO, &saved_tio) < 0) {
        return;
    }
    t = saved_tio;
    /* ISIG stays on: ctrl-C and ctrl-Z still raise their signals, and
     * the handlers below put the terminal back before acting on them. */
    t.c_lflag &= ~(u32)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
        raw_on = 1;
    }
}

void gfx_keys_restore(void)
{
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
        raw_on = 0;
    }
}

static u8 kbuf[32];
static u32 kn;

static void kfill(void)
{
    u32 avail = 0;
    s32 got;

    if (kn >= sizeof(kbuf) ||
        ioctl(STDIN_FILENO, FIONREAD, (u32)&avail) < 0 || avail == 0) {
        return;
    }
    if (avail > sizeof(kbuf) - kn) {
        avail = sizeof(kbuf) - kn;
    }
    got = read(STDIN_FILENO, kbuf + kn, avail);
    if (got > 0) {
        kn += (u32)got;
    }
}

static void kdrop(u32 n)
{
    u32 i;

    for (i = n; i < kn; i++) {
        kbuf[i - n] = kbuf[i];
    }
    kn -= n;
}

/*
 * The rest of an escape sequence arrives a moment after its ESC -- in
 * one burst from the keyboard driver, but a byte at a time down a slow
 * serial line. Wait briefly for `need` bytes before deciding the ESC
 * was a key of its own.
 */
static void kwait(u32 need)
{
    int tries;

    for (tries = 0; tries < 5 && kn < need; tries++) {
        msleep(10);
        kfill();
    }
}

int gfx_key(void)
{
    u32 i, num;
    u8 fin;

    kfill();
    if (kn == 0) {
        return GFX_NOKEY;
    }
    if (kbuf[0] != GFX_KEY_ESC) {
        int c = kbuf[0];

        kdrop(1);
        return c;
    }

    kwait(2);
    if (kn < 2 || (kbuf[1] != '[' && kbuf[1] != 'O')) {
        kdrop(1);
        return GFX_KEY_ESC;
    }

    /* ESC [ digits final, or ESC O final. */
    i = 2;
    num = 0;
    for (;;) {
        if (i >= kn) {
            kwait(i + 1);
            if (i >= kn) {
                kdrop(kn);          /* a sequence cut short: drop it */
                return GFX_NOKEY;
            }
        }
        if (kbuf[i] >= '0' && kbuf[i] <= '9') {
            num = num * 10 + (u32)(kbuf[i] - '0');
            i++;
            continue;
        }
        if (kbuf[i] == ';') {       /* a modifier: ignored */
            num = 0;
            i++;
            continue;
        }
        break;
    }
    fin = kbuf[i];
    kdrop(i + 1);

    switch (fin) {
    case 'A': return GFX_KEY_UP;
    case 'B': return GFX_KEY_DOWN;
    case 'C': return GFX_KEY_RIGHT;
    case 'D': return GFX_KEY_LEFT;
    case 'H': return GFX_KEY_HOME;
    case 'F': return GFX_KEY_END;
    case '~':
        switch (num) {
        case 1: case 7: return GFX_KEY_HOME;
        case 4: case 8: return GFX_KEY_END;
        case 3:         return GFX_KEY_DEL;
        case 5:         return GFX_KEY_PGUP;
        case 6:         return GFX_KEY_PGDN;
        }
        break;
    }
    return GFX_NOKEY;               /* a sequence nothing here knows */
}

int gfx_key_wait(void)
{
    int k;

    while ((k = gfx_key()) == GFX_NOKEY) {
        msleep(10);
    }
    return k;
}

int gfx_quit_key(int k)
{
    return k == 'q' || k == 'Q' || k == GFX_KEY_ESC;
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

/* The screen open now, for the signal handlers to put right. */
static struct gfx *active;

static void blank(struct gfx *g)
{
    ioctl(g->fd, FBIO_CLEAR, 0);
    ioctl(g->fd, FBIO_FLIP, 0);
}

/* Let `sig` through again, and raise it: with the default action in
 * place, that is the kill or the stop the handler was standing in for,
 * taken on the way back from raise(). */
static void act_on(int sig)
{
    sigset_t s;

    signal(sig, SIG_DFL);
    sigemptyset(&s);
    sigaddset(&s, sig);
    sigprocmask(SIG_UNBLOCK, &s, 0);
    raise(sig);
}

/*
 * Killed: leave a blank screen and a working terminal, then die the way
 * the signal says -- so the shell still sees a program killed by
 * SIGINT, not one that exited.
 */
static void on_kill(int sig)
{
    if (active) {
        blank(active);
    }
    gfx_keys_restore();
    act_on(sig);
}

static void on_stop(int sig);

/*
 * ctrl-Z: give the terminal back while stopped, and take it again when
 * `fg` continues the program. The shell and its prompt may have written
 * on the screen meanwhile, which turns double buffering off; the next
 * frame redraws everything, and gfx_frame() turns it back on.
 */
static void on_stop(int sig)
{
    int was_raw = raw_on;

    gfx_keys_restore();
    act_on(sig);                    /* stops here, until SIGCONT */

    signal(SIGTSTP, on_stop);
    if (was_raw) {
        gfx_keys_raw();
    }
}

static void catch_signals(int on)
{
    signal(SIGINT, on ? on_kill : SIG_DFL);
    signal(SIGTERM, on ? on_kill : SIG_DFL);
    signal(SIGHUP, on ? on_kill : SIG_DFL);
    signal(SIGTSTP, on ? on_stop : SIG_DFL);
}

int gfx_open(struct gfx *g, const char *prog, int flags)
{
    g->prog = prog;
    g->map = 0;
    g->map_len = 0;

    g->fd = open("/dev/fb0", O_RDWR);
    if (g->fd < 0) {
        complain(prog, "no /dev/fb0", "", "");
        return -1;
    }
    if (ioctl(g->fd, FBIO_GETINFO, (u32)&g->info) < 0 || g->info.bpp != 8) {
        complain(prog, "/dev/fb0 is not an 8-bit screen", "", "");
        close(g->fd);
        return -1;
    }
    /*
     * Double buffering is not the framebuffer's default state: the text
     * console turns it off, because a console draws a character at a
     * time and each one has to appear.
     */
    if (ioctl(g->fd, FBIO_DOUBLE, 1) < 0) {
        complain(prog, "/dev/fb0 cannot double buffer", "", "");
        close(g->fd);
        return -1;
    }
    ioctl(g->fd, FBIO_GETINFO, (u32)&g->info);

    if (flags & GFX_MAP) {
        /* The mapping is video memory itself, not a copy: it costs its
         * page tables and no RAM (see apps/fbmap.c). */
        g->map_len = g->info.mem_size;
        g->map = mmap(0, g->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                      g->fd, 0);
        if (g->map == MAP_FAILED) {
            complain(prog, "cannot map /dev/fb0", "", "");
            g->map = 0;
            close(g->fd);
            return -1;
        }
    }

    active = g;
    catch_signals(1);
    gfx_keys_raw();
    return 0;
}

u8 *gfx_frame(struct gfx *g)
{
    ioctl(g->fd, FBIO_DOUBLE, 1);   /* a no-op unless the console wrote */
    ioctl(g->fd, FBIO_GETINFO, (u32)&g->info);
    return g->map ? g->map + g->info.draw_offset : 0;
}

void gfx_flip(struct gfx *g)
{
    ioctl(g->fd, FBIO_FLIP, 0);
}

void gfx_clear(struct gfx *g, u32 colour)
{
    ioctl(g->fd, FBIO_CLEAR, colour);
}

void gfx_line(struct gfx *g, int x0, int y0, int x1, int y1, u32 colour)
{
    struct fb_line l;

    l.x0 = x0; l.y0 = y0;
    l.x1 = x1; l.y1 = y1;
    l.colour = colour;
    ioctl(g->fd, FBIO_LINE, (u32)&l);
}

void gfx_close(struct gfx *g)
{
    /* A blank screen rather than the last frame frozen on it, which
     * looks like a machine that has hung. */
    blank(g);
    gfx_keys_restore();
    if (active == g) {
        catch_signals(0);
        active = 0;
    }
    if (g->map) {
        munmap(g->map, g->map_len);
        g->map = 0;
    }
    close(g->fd);
    g->fd = -1;
}

void gfx_copy_row(u8 *dst, const u8 *src, u32 n)
{
    if ((((u32)dst | (u32)src) & 3) == 0) {
        u32 *d = (u32 *)dst;
        const u32 *s = (const u32 *)src;
        u32 w = n >> 2;

        while (w--) {
            *d++ = *s++;
        }
        dst = (u8 *)d;
        src = (const u8 *)s;
        n &= 3;
    }
    while (n--) {
        *dst++ = *src++;
    }
}

void gfx_fill_row(u8 *dst, u8 colour, u32 n)
{
    u32 four = colour * 0x01010101UL;

    while (n && ((u32)dst & 3)) {
        *dst++ = colour;
        n--;
    }
    while (n >= 4) {
        *(u32 *)dst = four;
        dst += 4;
        n -= 4;
    }
    while (n--) {
        *dst++ = colour;
    }
}

/* ---------------------------------------------------------------- */
/* Colour                                                            */
/* ---------------------------------------------------------------- */

void gfx_colour(struct gfx *g, u32 index, u32 rgb)
{
    struct fb_palette p;

    p.index = index;
    p.rgb = rgb;
    ioctl(g->fd, FBIO_PALETTE, (u32)&p);
}

u32 gfx_blend(u32 a, u32 b, u32 num, u32 den)
{
    u32 r = 0;
    int shift;

    for (shift = 0; shift <= 16; shift += 8) {
        u32 ca = (a >> shift) & 255, cb = (b >> shift) & 255;

        r |= ((ca * (den - num) + cb * num) / den) << shift;
    }
    return r;
}

void gfx_ramp(struct gfx *g, u32 first, u32 n,
              const u32 *keys, u32 nkeys, int cyclic)
{
    /*
     * Entry i sits i/den of the way along `spans` spans: span k, a
     * fraction f/den through it. Straight, den is n - 1 so the last
     * entry lands exactly on the last key; cyclic, den is n and the last
     * span runs from the last key back to the first.
     */
    u32 spans = cyclic ? nkeys : nkeys - 1;
    u32 den = cyclic ? n : n - 1;
    u32 i;

    for (i = 0; i < n; i++) {
        u32 pos = i * spans;
        u32 k = den ? pos / den : 0, f = den ? pos % den : 0;
        u32 rgb;

        if (nkeys < 2 || k >= spans) {
            rgb = keys[nkeys < 2 ? 0 : nkeys - 1];
        } else {
            rgb = gfx_blend(keys[k], keys[(k + 1) % nkeys], f, den);
        }
        gfx_colour(g, first + i, rgb);
    }
}

/* ---------------------------------------------------------------- */
/* Frame pacing                                                      */
/* ---------------------------------------------------------------- */

void gfx_clock_start(struct gfx_clock *c, u32 fps)
{
    c->fps = fps;
    c->start = c->started = times(0);
    c->n = 0;
    c->frames = 0;
}

void gfx_clock_tick(struct gfx_clock *c)
{
    u32 due, now;
    s32 ahead;

    c->frames++;
    if (c->fps == 0) {
        return;
    }
    c->n++;
    due = c->start + (c->n * HZ) / c->fps;
    now = times(0);
    ahead = (s32)(due - now);
    if (ahead > 0) {
        msleep((u32)ahead * 1000 / HZ);
    } else if (-ahead > (s32)(HZ / c->fps) + 1) {
        c->start = now;
        c->n = 0;
    }
}

u32 gfx_clock_pause(struct gfx_clock *c)
{
    (void)c;
    return times(0);
}

void gfx_clock_resume(struct gfx_clock *c, u32 paused_at)
{
    u32 now = times(0);

    c->started += now - paused_at;
    c->start = now;
    c->n = 0;
}

void gfx_clock_summary(const struct gfx_clock *c, const char *prog,
                       const char *noun)
{
    u32 elapsed = times(0) - c->started;

    puts(prog);
    puts(": ");
    putdec(c->frames);
    putch(' ');
    puts(noun);
    puts(" in ");
    putdec(elapsed / HZ);
    puts(" seconds");
    if (elapsed > 0) {
        puts(" (");
        putdec(c->frames * HZ / elapsed);
        puts(" per second)");
    }
}
