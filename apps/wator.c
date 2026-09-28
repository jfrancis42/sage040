/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * wator.c - Wa-Tor, A. K. Dewdney's predator and prey planet
 * (Scientific American, "Computer Recreations", December 1984).
 *
 *     sage$ wator [options]      (wator -h lists them, with defaults)
 *
 * A toroidal ocean of fish and sharks. Each chronon every creature,
 * taken in random order, gets one turn:
 *
 *   a fish     moves to a random empty neighbouring cell, if there is
 *              one; once it is old enough it leaves a newborn fish
 *              behind where it was.
 *   a shark    eats a random neighbouring fish if there is one and moves
 *              into its cell; otherwise, if it has gone too long without
 *              eating it starves, and if it has not it moves like a
 *              fish. It breeds the way a fish does.
 *
 * Neighbours are the four cells north, south, east and west. Nothing
 * else is modelled, and the two populations still chase each other
 * round the cycle Lotka and Volterra described: fish boom, sharks boom
 * on the fish, the fish crash, the sharks starve, the fish recover. The
 * strip along the bottom of the screen plots both, each scaled to its
 * own maximum over the strip so that the lag between them shows.
 *
 * Drawn by mapping /dev/fb0 and writing bytes, because an ocean of
 * sixteen thousand cells is sixteen thousand ioctls a frame drawn any
 * other way. The mapping is of video memory itself (see fbmap.c) and
 * takes no RAM.
 */
#include "ulib.h"
#include "malloc.h"

#define WATER       0
#define FISH        1
#define SHARK       2

/* Palette entries 32 up: 0-7 are the driver's and 16-31 the console's,
 * and neither is changed. */
#define COL_WATER   32
#define COL_FISH    33
#define COL_SHARK   34
#define COL_GRAPH   35
#define COL_RULE    36

static const u32 colours[5] = {
    0x000c24UL,                 /* water: deep navy                  */
    0x40e040UL,                 /* fish                              */
    0xff5030UL,                 /* shark                             */
    0x080808UL,                 /* graph background                  */
    0x405070UL,                 /* the rule above the graph          */
};

static u32 rng_state;

/* xorshift32: small, fast, and a seed can be given to repeat a run. */
static u32 rnd(void)
{
    u32 x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* ---------------------------------------------------------------- */
/* Parameters                                                        */
/* ---------------------------------------------------------------- */

struct param {
    char opt;
    const char *name;
    u32 value;
    u32 min, max;
    const char *unit;
};

enum {
    P_FISH, P_SHARKS, P_FBREED, P_SBREED, P_STARVE, P_CELL,
    P_GRAPH, P_RESTART, P_PRINT, P_FPS, P_NPARAM
};

/*
 * The defaults keep both species alive through cycle after cycle on
 * the default ocean (160 x 100) -- over 3,400 chronons in testing, with
 * the fish swinging between about 4,800 and 10,600 and the sharks
 * between 900 and 1,900. Dewdney's own 3/10/3 survives too, but its
 * cycles are damped almost flat. Sharks that breed faster, or starve
 * slower, eat the fish out and then die themselves; that is worth
 * seeing too, and -R restarts the ocean when it happens.
 */
static struct param par[P_NPARAM] = {
    [P_FISH]    = { 'f', "initial fish",               30, 0, 100,
                    "% of the ocean" },
    [P_SHARKS]  = { 's', "initial sharks",              5, 0, 100,
                    "% of the ocean" },
    [P_FBREED]  = { 'b', "fish breeding age",           3, 1, 255,
                    "chronons" },
    [P_SBREED]  = { 'B', "shark breeding age",         12, 1, 255,
                    "chronons" },
    [P_STARVE]  = { 't', "shark starvation time",       4, 1, 255,
                    "chronons" },
    [P_CELL]    = { 'z', "cell size",                   4, 1, 32, "px" },
    [P_GRAPH]   = { 'g', "population graph height",    80, 0, 400,
                    "px, 0 for none" },
    [P_RESTART] = { 'R', "restart when a species dies out", 1, 0, 1,
                    "0 or 1" },
    [P_PRINT]   = { 'p', "print populations every",     0, 0, 100000,
                    "chronons, 0 for never" },
    [P_FPS]     = { 'F', "chronons per second",        20, 0, 200,
                    "0 for as fast as it will go" },
};

static u32 seed;                /* -x: 0 means "from the clock"     */

/* To stdout: it is only printed when -h asks for it. */
static void usage(void)
{
    int i;

    puts("usage: wator [-x seed] [-OPT value ...]\n");
    for (i = 0; i < P_NPARAM; i++) {
        char buf[4] = { ' ', '-', par[i].opt, '\0' };

        puts(buf);
        puts("  ");
        puts(par[i].name);
        if (par[i].unit[0]) {
            puts(", ");
            puts(par[i].unit);
        }
        puts(" (default ");
        putdec(par[i].value);
        puts(")\n");
    }
    puts(" -x  random seed (default: from the clock)\n");
    puts(" -h  this\n");
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

/* -f 30 and -f30 both work. */
static int parse_args(int argc, char **argv)
{
    int i, k;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val;
        u32 v;

        if (a[0] != '-' || a[1] == '\0') {
            return -1;
        }
        if (a[1] == 'h' && a[2] == '\0') {
            usage();
            return 1;
        }

        val = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
        if (!val || parse_u32(val, &v) < 0) {
            eputs("wator: -");
            eputs(a + 1);
            eputs(" wants a number\n");
            return -1;
        }
        if (a[1] == 'x') {
            seed = v;
            continue;
        }
        for (k = 0; k < P_NPARAM; k++) {
            if (par[k].opt == a[1]) {
                break;
            }
        }
        if (k == P_NPARAM) {
            eputs("wator: unknown option ");
            eputs(a);
            eputs("\n");
            return -1;
        }
        if (v < par[k].min || v > par[k].max) {
            eputs("wator: ");
            eputs(par[k].name);
            eputs(" out of range\n");
            return -1;
        }
        par[k].value = v;
    }

    if (par[P_FISH].value + par[P_SHARKS].value > 100) {
        eputs("wator: more than 100% of the ocean asked for\n");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* The ocean                                                         */
/* ---------------------------------------------------------------- */

static int gw, gh;              /* the ocean, in cells               */
static u32 ncells;
static u8 *kind;                /* WATER, FISH or SHARK              */
static u8 *age;                 /* chronons since born or last bred  */
static u8 *hunger;              /* a shark's chronons since a meal   */
static u32 *moved;              /* the chronon it last acted in      */
static u32 *order;              /* this chronon's turn order         */
static u32 chronon;
static u32 nfish, nsharks;

static int alloc_ocean(void)
{
    kind   = malloc(ncells);
    age    = malloc(ncells);
    hunger = malloc(ncells);
    moved  = malloc(ncells * sizeof(u32));
    order  = malloc(ncells * sizeof(u32));
    return kind && age && hunger && moved && order ? 0 : -1;
}

/*
 * Stock the ocean: exactly the asked-for number of each, placed by
 * shuffling, and each with a random age so that the whole first
 * generation does not breed in the same chronon.
 */
static void stock(void)
{
    u32 i, j, t;
    u32 f = ncells * par[P_FISH].value / 100;
    u32 s = ncells * par[P_SHARKS].value / 100;

    for (i = 0; i < ncells; i++) {
        order[i] = i;
    }
    for (i = ncells - 1; i > 0; i--) {
        j = rnd() % (i + 1);
        t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (i = 0; i < ncells; i++) {
        u32 c = order[i];

        kind[c] = i < f ? FISH : i < f + s ? SHARK : WATER;
        age[c] = 0;
        hunger[c] = 0;
        moved[c] = 0;
        if (kind[c] == FISH) {
            age[c] = (u8)(rnd() % par[P_FBREED].value);
        } else if (kind[c] == SHARK) {
            age[c] = (u8)(rnd() % par[P_SBREED].value);
            hunger[c] = (u8)(rnd() % par[P_STARVE].value);
        }
    }
    nfish = f;
    nsharks = s;
    chronon = 0;
}

/* The four neighbours of cell c, north, south, east, west, round the
 * torus. */
static void neighbours(u32 c, u32 n[4])
{
    int x = (int)(c % (u32)gw), y = (int)(c / (u32)gw);
    int xl = x ? x - 1 : gw - 1, xr = x + 1 < gw ? x + 1 : 0;
    int yu = y ? y - 1 : gh - 1, yd = y + 1 < gh ? y + 1 : 0;

    n[0] = (u32)(yu * gw + x);
    n[1] = (u32)(yd * gw + x);
    n[2] = (u32)(y * gw + xr);
    n[3] = (u32)(y * gw + xl);
}

/* A random neighbour of c holding `what`, or -1 if none does. */
static s32 pick(u32 c, u8 what)
{
    u32 n[4], hit[4];
    int i, k = 0;

    neighbours(c, n);
    for (i = 0; i < 4; i++) {
        if (kind[n[i]] == what) {
            hit[k++] = n[i];
        }
    }
    return k ? (s32)hit[rnd() % (u32)k] : -1;
}

/*
 * Move whatever is in `from` to `to`, and if it is old enough to breed,
 * leave a newborn of its kind behind. Both cells are marked as having
 * acted, so neither the mover nor the baby gets a second turn if the
 * shuffled order reaches them later in this chronon.
 */
static void move_to(u32 from, u32 to, u8 breed_age)
{
    u8 k = kind[from];

    kind[to] = k;
    age[to] = age[from];
    hunger[to] = hunger[from];
    moved[to] = chronon;

    if (age[to] >= breed_age) {
        age[to] = 0;
        age[from] = 0;
        hunger[from] = 0;
        moved[from] = chronon;
        if (k == FISH) {
            nfish++;
        } else {
            nsharks++;
        }
    } else {
        kind[from] = WATER;
    }
}

static void fish_turn(u32 c)
{
    s32 to;

    if (age[c] < 255) age[c]++;
    to = pick(c, WATER);
    if (to >= 0) {
        move_to(c, (u32)to, (u8)par[P_FBREED].value);
    }
    /* A fish with nowhere to go neither moves nor breeds -- Dewdney's
     * rule, and what stops a full ocean filling past full. */
}

static void shark_turn(u32 c)
{
    s32 to;

    if (age[c] < 255) age[c]++;
    if (hunger[c] < 255) hunger[c]++;

    to = pick(c, FISH);
    if (to >= 0) {
        nfish--;
        hunger[c] = 0;
        move_to(c, (u32)to, (u8)par[P_SBREED].value);
        return;
    }
    if (hunger[c] >= par[P_STARVE].value) {
        kind[c] = WATER;
        nsharks--;
        return;
    }
    to = pick(c, WATER);
    if (to >= 0) {
        move_to(c, (u32)to, (u8)par[P_SBREED].value);
    }
}

/*
 * One chronon. The order is shuffled every time: sweeping the grid in
 * the same order each chronon would let the creatures early in the
 * sweep always move first, and the whole ocean drifts against the
 * direction of the sweep.
 */
static void step(void)
{
    u32 i, j, t;

    chronon++;
    for (i = ncells - 1; i > 0; i--) {
        j = rnd() % (i + 1);
        t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (i = 0; i < ncells; i++) {
        u32 c = order[i];

        if (moved[c] == chronon) {
            continue;
        }
        if (kind[c] == FISH) {
            fish_turn(c);
        } else if (kind[c] == SHARK) {
            shark_turn(c);
        }
    }
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

static int fb = -1;
static struct fb_info info;
static u8 *map;                 /* all of video memory, mapped       */
static u32 map_len;
static u8 rowbuf[1024] __attribute__((aligned(4)));

/* The graph's history, one sample per pixel column, oldest first
 * starting at hist_at. */
static u32 *hist_fish, *hist_shark;
static u32 hist_at, hist_n;

static void make_palette(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        struct fb_palette p;

        p.index = (u32)(COL_WATER + i);
        p.rgb = colours[i];
        ioctl(fb, FBIO_PALETTE, (u32)&p);
    }
}

/* A row of pixels into video memory, a longword at a time where it can
 * be: ulib's memcpy goes a byte at a time, and this is most of what a
 * frame costs. */
static void put_row(u8 *dst, const u8 *src, u32 n)
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

static void fill_row(u8 *dst, u8 colour, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        rowbuf[i] = colour;
    }
    put_row(dst, rowbuf, n);
}

static void record(void)
{
    u32 w = info.width;
    u32 at = (hist_at + hist_n) % w;

    hist_fish[at] = nfish;
    hist_shark[at] = nsharks;
    if (hist_n < w) {
        hist_n++;
    } else {
        hist_at = (hist_at + 1) % w;
    }
}

/* One series, as a line: each column joined to the one before it by a
 * vertical run, so that a steep change still reads as a line and not a
 * scatter of dots. */
static void plot(u8 *top, u32 h, const u32 *hist, u8 colour)
{
    u32 i, max = 1;
    int prev = -1;

    for (i = 0; i < hist_n; i++) {
        u32 v = hist[(hist_at + i) % info.width];

        if (v > max) max = v;
    }
    for (i = 0; i < hist_n; i++) {
        u32 v = hist[(hist_at + i) % info.width];
        int y = (int)(h - 1) - (int)(v * (h - 1) / max);
        int a = prev < 0 ? y : prev, b = y, t;

        if (a > b) { t = a; a = b; b = t; }
        for (t = a; t <= b; t++) {
            top[(u32)t * info.pitch + i] = colour;
        }
        prev = y;
    }
}

static void render(void)
{
    u32 cell = par[P_CELL].value;
    u32 gpx = par[P_GRAPH].value;
    u32 ocean_px = (u32)gh * cell;
    u32 x, y, k;
    u8 *draw;

    /* The buffer to draw in changes with every flip. */
    ioctl(fb, FBIO_GETINFO, (u32)&info);
    draw = map + info.draw_offset;

    for (y = 0; y < (u32)gh; y++) {
        const u8 *row = kind + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = (u8)(COL_WATER + row[x]);

            for (k = 0; k < cell; k++) {
                *p++ = c;
            }
        }
        while (p < rowbuf + info.width) {
            *p++ = COL_WATER;
        }
        for (k = 0; k < cell; k++) {
            put_row(draw + (y * cell + k) * info.pitch, rowbuf, info.width);
        }
    }
    /* What the cells do not cover, between the ocean and the graph. */
    for (y = ocean_px; y < info.height - gpx; y++) {
        fill_row(draw + y * info.pitch, COL_WATER, info.width);
    }

    if (gpx) {
        u8 *top = draw + (info.height - gpx) * info.pitch;

        fill_row(top, COL_RULE, info.width);
        for (y = 1; y < gpx; y++) {
            fill_row(top + y * info.pitch, COL_GRAPH, info.width);
        }
        top += info.pitch;
        plot(top, gpx - 1, hist_fish, COL_FISH);
        plot(top, gpx - 1, hist_shark, COL_SHARK);
    }

    ioctl(fb, FBIO_FLIP, 0);
}

/*
 * Sleep until frame `n` is due, counting from the tick `*start` -- see
 * boids.c, where this came from. A deadline measured from the start
 * lets one frame's rounding to the tick be made up by the next; a fall
 * of more than a frame behind restarts the schedule rather than racing.
 */
static void frame_wait(u32 *start, u32 *n, u32 fps)
{
    u32 due, now;
    s32 ahead;

    (*n)++;
    due = *start + (*n * HZ) / fps;
    now = times(0);
    ahead = (s32)(due - now);
    if (ahead > 0) {
        msleep((u32)ahead * 1000 / HZ);
    } else if (-ahead > (s32)(HZ / fps) + 1) {
        *start = now;
        *n = 0;
    }
}

static void report(const char *what)
{
    puts("wator: ");
    puts(what);
    puts("chronon ");
    putdec(chronon);
    puts(", ");
    putdec(nfish);
    puts(" fish, ");
    putdec(nsharks);
    puts(" sharks\n");
}

int main(int argc, char **argv)
{
    u32 frames = 0, oceans = 1;
    u32 sched = 0, sched_start;
    u32 started;
    u32 fps, every;
    int r;

    r = parse_args(argc, argv);
    if (r > 0) {
        return 0;
    }
    if (r < 0) {
        eputs("wator -h lists the options\n");
        return 2;
    }
    fps = par[P_FPS].value;
    every = par[P_PRINT].value;

    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        eputs("wator: no /dev/fb0\n");
        return 1;
    }
    if (ioctl(fb, FBIO_GETINFO, (u32)&info) < 0 || info.bpp != 8 ||
        info.width > sizeof(rowbuf)) {
        eputs("wator: /dev/fb0 is not an 8-bit screen this can draw on\n");
        close(fb);
        return 1;
    }
    if (par[P_GRAPH].value + par[P_CELL].value > info.height) {
        eputs("wator: the graph leaves no room for an ocean\n");
        close(fb);
        return 1;
    }
    if (ioctl(fb, FBIO_DOUBLE, 1) < 0) {
        eputs("wator: /dev/fb0 cannot double buffer\n");
        close(fb);
        return 1;
    }
    ioctl(fb, FBIO_GETINFO, (u32)&info);
    map_len = info.mem_size;
    map = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (map == MAP_FAILED) {
        eputs("wator: cannot map /dev/fb0\n");
        close(fb);
        return 1;
    }

    gw = (int)(info.width / par[P_CELL].value);
    gh = (int)((info.height - par[P_GRAPH].value) / par[P_CELL].value);
    ncells = (u32)gw * (u32)gh;
    hist_fish = malloc(info.width * sizeof(u32));
    hist_shark = malloc(info.width * sizeof(u32));
    if (alloc_ocean() < 0 || !hist_fish || !hist_shark) {
        eputs("wator: not enough memory for that ocean\n");
        return 1;
    }

    if (seed == 0) {
        seed = (u32)time(0) ^ times(0);
    }
    rng_state = seed ? seed : 1;    /* xorshift is stuck at 0 */

    make_palette();
    stock();

    puts("wator: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(" ocean, ");
    putdec(nfish);
    puts(" fish, ");
    putdec(nsharks);
    puts(" sharks, seed ");
    putdec(seed);
    puts("\npress any key to stop\n");

    started = times(0);
    sched_start = started;

    while (!key_waiting()) {
        record();
        render();
        step();
        frames++;

        if (every && chronon % every == 0) {
            report("");
        }
        /* Only a species that was stocked can die out: -s 0 is an
         * ocean of fish on purpose, not a failure to restart from. */
        if (par[P_RESTART].value &&
            ((par[P_SHARKS].value && nsharks == 0) ||
             (par[P_FISH].value && nfish == 0))) {
            report(nsharks == 0 ? "the sharks died out at "
                                : "the fish died out at ");
            stock();
            hist_at = hist_n = 0;
            oceans++;
        }
        if (fps) {
            frame_wait(&sched_start, &sched, fps);
        }
    }

    {
        char c;

        read(STDIN_FILENO, &c, 1);
    }

    ioctl(fb, FBIO_CLEAR, 0);
    ioctl(fb, FBIO_FLIP, 0);
    munmap(map, map_len);
    close(fb);

    report("stopped at ");
    {
        u32 elapsed = times(0) - started;

        puts("wator: ");
        putdec(frames);
        puts(" chronons in ");
        putdec(elapsed / HZ);
        puts(" seconds");
        if (elapsed > 0) {
            puts(" (");
            putdec(frames * HZ / elapsed);
            puts(" per second)");
        }
        if (oceans > 1) {
            puts(", ");
            putdec(oceans);
            puts(" oceans");
        }
        putch('\n');
    }
    return 0;
}
