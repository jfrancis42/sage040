/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * life.c - John Conway's Game of Life (Scientific American, "Mathematical
 * Games", October 1970), and the other "Life-like" rules.
 *
 *     sage$ life [options]       (life -h lists them, with defaults)
 *
 * A toroidal grid of cells, each alive or dead. Every generation, all
 * at once, each cell counts its eight live neighbours: a dead cell with
 * a count in the rule's B set is born, a live one with a count in its S
 * set survives, and everything else is dead. Conway's rule is B3/S23;
 * -r takes any other in the same notation, which is how HighLife
 * (B36/S23, with its replicator), Day & Night (B3678/S34678), Seeds
 * (B2/S) and Maze (B3/S12345) are all one option away.
 *
 * A live cell is coloured by its age -- white when it is born, through
 * yellow and orange to indigo when it has sat still a long time -- so
 * still lifes settle into blue while oscillators and anything moving
 * stay bright. A cell that has died leaves a trail that fades over a
 * few generations, which shows where things have BEEN: a glider draws
 * its own path.
 *
 * When the board has settled -- repeating itself exactly, or its
 * population flat for long enough that nothing is happening but
 * oscillators and gliders -- it starts again, unless -R 0 says not to.
 *
 * Drawn the way wator is, by mapping /dev/fb0 and writing bytes; see
 * that program for why.
 */
#include "ulib.h"
#include "malloc.h"

#define COL_MONO    1           /* green, in the driver's palette    */
#define PAL_LIVE    32          /* 16 ages, 32-47                    */
#define NLIVE       16
#define PAL_TRAIL   48          /* 8 fading trail levels, 48-55      */
#define NTRAIL      8

/*
 * How settled counts as settled. A repeat of the whole board within
 * HASH_RING generations, held for SETTLE_REPEAT generations; or a
 * population that has moved by no more than FLAT_BAND cells over
 * FLAT_GENS generations. The second catches ash with gliders still
 * flying through it, which on a torus can take thousands of generations
 * to repeat exactly.
 */
#define HASH_RING       32
#define SETTLE_REPEAT   60
#define FLAT_GENS       500
#define FLAT_BAND       12

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
/* Patterns                                                          */
/* ---------------------------------------------------------------- */

struct pattern {
    const char *name;
    const char *what;
    const char *const *rows;    /* 'X' alive; 0 = a random soup      */
};

static const char *const p_rpent[] = {
    ".XX",
    "XX.",
    ".X.",
    0
};

static const char *const p_acorn[] = {
    ".X.....",
    "...X...",
    "XX..XXX",
    0
};

static const char *const p_diehard[] = {
    "......X.",
    "XX......",
    ".X...XXX",
    0
};

static const char *const p_gun[] = {
    "........................X...........",
    "......................X.X...........",
    "............XX......XX............XX",
    "...........X...X....XX............XX",
    "XX........X.....X...XX..............",
    "XX........X...X.XX....X.X...........",
    "..........X.....X.......X...........",
    "...........X...X....................",
    "............XX......................",
    0
};

static const char *const p_line[] = {
    "XXXXXXXX.XXXXX...XXX......XXXXXXX.XXXXX",
    0
};

static const struct pattern patterns[] = {
    { "random",  "a random soup at the -d density",               0 },
    { "rpent",   "R-pentomino: 5 cells, 1103 generations of chaos", p_rpent },
    { "acorn",   "acorn: 7 cells, 5206 generations",             p_acorn },
    { "diehard", "diehard: vanishes after 130 generations",      p_diehard },
    { "gun",     "Gosper glider gun, on a torus its own gliders hit", p_gun },
    { "line",    "one row that grows without limit",             p_line },
};
#define NPATTERNS (sizeof(patterns) / sizeof(patterns[0]))

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
    P_CELL, P_DENSITY, P_TRAIL, P_COLOUR, P_RESTART, P_PRINT, P_FPS,
    P_NPARAM
};

static struct param par[P_NPARAM] = {
    [P_CELL]    = { 'z', "cell size",                  4, 1, 32, "px" },
    [P_DENSITY] = { 'd', "random soup density",       25, 1, 100, "%" },
    [P_TRAIL]   = { 'T', "trail length",               8, 0, 255,
                    "generations, 0 for none" },
    [P_COLOUR]  = { 'c', "colour: 0 mono, 1 by age",   1, 0, 1, "" },
    [P_RESTART] = { 'R', "restart when settled",       1, 0, 1, "0 or 1" },
    [P_PRINT]   = { 'v', "print the population every", 0, 0, 100000,
                    "generations, 0 for never" },
    [P_FPS]     = { 'F', "generations per second",    20, 0, 200,
                    "0 for as fast as it will go" },
};

static u32 seed;                /* -x: 0 means "from the clock"     */
static const char *rule_text = "B3/S23";
static u32 birth, survive;      /* bit n: a count of n qualifies    */
static const struct pattern *pattern = &patterns[0];

/* To stdout: it is only printed when -h asks for it. */
static void usage(void)
{
    u32 i;

    puts("usage: life [-r RULE] [-p PATTERN] [-x seed] [-OPT value ...]\n");
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
    puts(" -r  rule, as B<born>/S<survives> (default B3/S23, Conway's);\n"
         "     try B36/S23, B3678/S34678, B2/S, B3/S12345\n");
    puts(" -p  starting pattern (default random):\n");
    for (i = 0; i < NPATTERNS; i++) {
        puts("       ");
        puts(patterns[i].name);
        puts("  ");
        puts(patterns[i].what);
        putch('\n');
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

/*
 * "B3/S23": digits after B are the counts that give birth, after S the
 * counts that let a cell survive. Either half may be empty (Seeds is
 * B2/S) and the case of the letters does not matter.
 */
static int parse_rule(const char *s)
{
    u32 *set = 0;

    birth = survive = 0;
    for (; *s; s++) {
        if (*s == 'B' || *s == 'b') {
            set = &birth;
        } else if (*s == 'S' || *s == 's') {
            set = &survive;
        } else if (*s == '/') {
            continue;
        } else if (*s >= '0' && *s <= '8' && set) {
            *set |= 1UL << (*s - '0');
        } else {
            return -1;
        }
    }
    /* B0 would have every empty cell born in the first generation --
     * a different kind of automaton, which this does not draw. */
    return set && !(birth & 1) ? 0 : -1;
}

/* -z 4 and -z4 both work. */
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
        if (!val) {
            eputs("life: -");
            eputs(a + 1);
            eputs(" wants a value\n");
            return -1;
        }
        if (a[1] == 'r') {
            rule_text = val;
            continue;
        }
        if (a[1] == 'p') {
            u32 n;

            for (n = 0; n < NPATTERNS; n++) {
                if (strcmp(val, patterns[n].name) == 0) {
                    break;
                }
            }
            if (n == NPATTERNS) {
                eputs("life: no pattern called ");
                eputs(val);
                eputs("\n");
                return -1;
            }
            pattern = &patterns[n];
            continue;
        }
        if (parse_u32(val, &v) < 0) {
            eputs("life: -");
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
            eputs("life: unknown option ");
            eputs(a);
            eputs("\n");
            return -1;
        }
        if (v < par[k].min || v > par[k].max) {
            eputs("life: ");
            eputs(par[k].name);
            eputs(" out of range\n");
            return -1;
        }
        par[k].value = v;
    }

    if (parse_rule(rule_text) < 0) {
        eputs("life: cannot read the rule ");
        eputs(rule_text);
        eputs(" -- it wants the form B3/S23, and no B0\n");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* The board                                                         */
/* ---------------------------------------------------------------- */

/*
 * Two boards with a border of one cell all round, so that a cell's
 * eight neighbours are always at the same eight offsets. The border is
 * a copy of the far edge, remade every generation (wrap_edges), which
 * is what makes the board a torus without a modulo in the inner loop.
 */
static int gw, gh, stride;
static u8 *cur, *nxt;
static u8 *age;                 /* live: generations alive;         */
                                /* dead: generations since it died  */
static u32 generation, population;

static u32 hashes[HASH_RING];
static u32 pops[FLAT_GENS];
static u32 repeats;

static int alloc_board(void)
{
    u32 n = (u32)stride * (u32)(gh + 2);

    cur = calloc(n, 1);
    nxt = calloc(n, 1);
    age = malloc((u32)gw * (u32)gh);
    return cur && nxt && age ? 0 : -1;
}

static void seed_board(void)
{
    u32 i, n = (u32)stride * (u32)(gh + 2);
    int x, y;

    memset(cur, 0, n);
    /* Everything starts as long dead: no trails from before time. */
    memset(age, 255, (u32)gw * (u32)gh);
    population = 0;

    if (!pattern->rows) {
        for (y = 1; y <= gh; y++) {
            for (x = 1; x <= gw; x++) {
                if (rnd() % 100 < par[P_DENSITY].value) {
                    cur[y * stride + x] = 1;
                    age[(y - 1) * gw + (x - 1)] = 0;
                    population++;
                }
            }
        }
    } else {
        int pw = (int)strlen(pattern->rows[0]), ph = 0;

        while (pattern->rows[ph]) {
            ph++;
        }
        for (y = 0; y < ph; y++) {
            for (x = 0; x < pw && pattern->rows[y][x]; x++) {
                /* Centred, and wrapped if the board is smaller. */
                int cx = ((gw - pw) / 2 + x) % gw;
                int cy = ((gh - ph) / 2 + y) % gh;

                if (cx < 0) cx += gw;
                if (cy < 0) cy += gh;
                if (pattern->rows[y][x] == 'X' &&
                    !cur[(cy + 1) * stride + cx + 1]) {
                    cur[(cy + 1) * stride + cx + 1] = 1;
                    age[cy * gw + cx] = 0;
                    population++;
                }
            }
        }
    }

    generation = 0;
    repeats = 0;
    for (i = 0; i < HASH_RING; i++) {
        hashes[i] = 0;
    }
}

static void wrap_edges(u8 *b)
{
    int y;

    memcpy(b, b + gh * stride, (u32)stride);                 /* top    */
    memcpy(b + (gh + 1) * stride, b + stride, (u32)stride);  /* bottom */
    for (y = 0; y < gh + 2; y++) {
        u8 *row = b + y * stride;

        row[0] = row[gw];
        row[gw + 1] = row[1];
    }
}

static void step(void)
{
    int x, y;
    u8 *t;
    u32 pop = 0;

    wrap_edges(cur);
    for (y = 1; y <= gh; y++) {
        const u8 *up = cur + (y - 1) * stride;
        const u8 *mid = cur + y * stride;
        const u8 *dn = cur + (y + 1) * stride;
        u8 *out = nxt + y * stride;
        u8 *ag = age + (y - 1) * gw - 1;

        for (x = 1; x <= gw; x++) {
            u32 n = (u32)(up[x - 1] + up[x] + up[x + 1] +
                          mid[x - 1] +        mid[x + 1] +
                          dn[x - 1] + dn[x] + dn[x + 1]);
            u8 was = mid[x];
            u8 now = (u8)(((was ? survive : birth) >> n) & 1);

            out[x] = now;
            if (now) {
                ag[x] = was ? (ag[x] < 255 ? ag[x] + 1 : 255) : 0;
                pop++;
            } else {
                ag[x] = was ? 0 : (ag[x] < 255 ? ag[x] + 1 : 255);
            }
        }
    }
    t = cur; cur = nxt; nxt = t;
    population = pop;
    generation++;
}

/* FNV-1a over the whole board. A collision could restart a board a
 * little early, which is harmless. */
static u32 board_hash(void)
{
    u32 h = 2166136261UL;
    int x, y;

    for (y = 1; y <= gh; y++) {
        const u8 *row = cur + y * stride;

        for (x = 1; x <= gw; x++) {
            h = (h ^ row[x]) * 16777619UL;
        }
    }
    return h;
}

/* Has nothing been happening for long enough to start again? */
static int settled(void)
{
    u32 h = board_hash();
    u32 i, lo, hi;
    int seen = 0;

    for (i = 0; i < HASH_RING; i++) {
        if (hashes[i] == h) {
            seen = 1;
            break;
        }
    }
    hashes[generation % HASH_RING] = h;
    repeats = seen ? repeats + 1 : 0;
    if (repeats >= SETTLE_REPEAT) {
        return 1;
    }

    pops[generation % FLAT_GENS] = population;
    if (generation < FLAT_GENS) {
        return 0;
    }
    lo = hi = pops[0];
    for (i = 1; i < FLAT_GENS; i++) {
        if (pops[i] < lo) lo = pops[i];
        if (pops[i] > hi) hi = pops[i];
    }
    return hi - lo <= FLAT_BAND;
}

/* ---------------------------------------------------------------- */
/* The screen                                                        */
/* ---------------------------------------------------------------- */

static int fb = -1;
static struct fb_info info;
static u8 *map;                 /* all of video memory, mapped       */
static u32 map_len;
static u8 rowbuf[1024] __attribute__((aligned(4)));
static u8 shade[256];           /* age -> palette entry, dead or alive */
static u8 shade_live[256];

/*
 * Young to old: white, yellow, orange, crimson, indigo. Entries between
 * the key colours are interpolated.
 */
static const u32 live_keys[5] = {
    0xffffffUL, 0xffff40UL, 0xff8030UL, 0xd03070UL, 0x5040c0UL
};

static u32 blend(u32 a, u32 b, u32 num, u32 den)
{
    u32 r = 0;
    int shift;

    for (shift = 0; shift <= 16; shift += 8) {
        u32 ca = (a >> shift) & 255, cb = (b >> shift) & 255;

        r |= ((ca * (den - num) + cb * num) / den) << shift;
    }
    return r;
}

static void set_colour(u32 index, u32 rgb)
{
    struct fb_palette p;

    p.index = index;
    p.rgb = rgb;
    ioctl(fb, FBIO_PALETTE, (u32)&p);
}

static void make_palette(void)
{
    u32 i, trail = par[P_TRAIL].value;
    int mono = par[P_COLOUR].value == 0;
    u32 ghost = mono ? 0x40ff40UL : 0x2080a0UL;

    for (i = 0; i < NLIVE; i++) {
        /* Sixteen entries over four spans between five keys. */
        u32 k = i * 4 / NLIVE, f = i * 4 % NLIVE;

        set_colour(PAL_LIVE + i,
                   blend(live_keys[k], live_keys[k + 1], f, NLIVE));
    }
    /* The trail starts at a third of the ghost colour's brightness and
     * fades to black. */
    for (i = 0; i < NTRAIL; i++) {
        u32 dim = blend(0, ghost, NTRAIL - i, NTRAIL * 3);

        set_colour(PAL_TRAIL + i, dim);
    }

    for (i = 0; i < 256; i++) {
        shade_live[i] = mono ? COL_MONO
                             : (u8)(PAL_LIVE + (i < NLIVE ? i : NLIVE - 1));
        /* A cell dead for `i` generations: a trail level, or nothing.
         * Age 0 is the generation it died in. */
        shade[i] = i < trail ? (u8)(PAL_TRAIL + i * NTRAIL / trail) : 0;
    }
}

/* See wator.c: a longword at a time, because ulib's memcpy is bytes. */
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

static void render(void)
{
    u32 cell = par[P_CELL].value;
    u32 x, y, k;
    u8 *draw;

    ioctl(fb, FBIO_GETINFO, (u32)&info);
    draw = map + info.draw_offset;

    for (y = 0; y < (u32)gh; y++) {
        const u8 *alive = cur + (y + 1) * (u32)stride + 1;
        const u8 *ag = age + y * (u32)gw;
        u8 *p = rowbuf;

        for (x = 0; x < (u32)gw; x++) {
            u8 c = alive[x] ? shade_live[ag[x]] : shade[ag[x]];

            for (k = 0; k < cell; k++) {
                *p++ = c;
            }
        }
        while (p < rowbuf + info.width) {
            *p++ = 0;
        }
        for (k = 0; k < cell; k++) {
            put_row(draw + (y * cell + k) * info.pitch, rowbuf, info.width);
        }
    }
    memset(rowbuf, 0, info.width);
    for (y = (u32)gh * cell; y < info.height; y++) {
        put_row(draw + y * info.pitch, rowbuf, info.width);
    }

    ioctl(fb, FBIO_FLIP, 0);
}

/*
 * Sleep until frame `n` is due, counting from the tick `*start` -- see
 * boids.c, where this came from.
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
    puts("life: ");
    puts(what);
    puts("generation ");
    putdec(generation);
    puts(", population ");
    putdec(population);
    putch('\n');
}

int main(int argc, char **argv)
{
    u32 frames = 0, boards = 1;
    u32 sched = 0, sched_start;
    u32 started;
    u32 fps, every;
    int r;

    r = parse_args(argc, argv);
    if (r > 0) {
        return 0;
    }
    if (r < 0) {
        eputs("life -h lists the options\n");
        return 2;
    }
    fps = par[P_FPS].value;
    every = par[P_PRINT].value;

    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) {
        eputs("life: no /dev/fb0\n");
        return 1;
    }
    if (ioctl(fb, FBIO_GETINFO, (u32)&info) < 0 || info.bpp != 8 ||
        info.width > sizeof(rowbuf)) {
        eputs("life: /dev/fb0 is not an 8-bit screen this can draw on\n");
        close(fb);
        return 1;
    }
    if (ioctl(fb, FBIO_DOUBLE, 1) < 0) {
        eputs("life: /dev/fb0 cannot double buffer\n");
        close(fb);
        return 1;
    }
    ioctl(fb, FBIO_GETINFO, (u32)&info);
    map_len = info.mem_size;
    map = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (map == MAP_FAILED) {
        eputs("life: cannot map /dev/fb0\n");
        close(fb);
        return 1;
    }

    gw = (int)(info.width / par[P_CELL].value);
    gh = (int)(info.height / par[P_CELL].value);
    stride = gw + 2;
    if (alloc_board() < 0) {
        eputs("life: not enough memory for that board\n");
        return 1;
    }

    if (seed == 0) {
        seed = (u32)time(0) ^ times(0);
    }
    rng_state = seed ? seed : 1;    /* xorshift is stuck at 0 */

    make_palette();
    seed_board();

    puts("life: ");
    putdec((u32)gw);
    putch('x');
    putdec((u32)gh);
    puts(" board, rule ");
    puts(rule_text);
    puts(", ");
    puts(pattern->name);
    puts(", seed ");
    putdec(seed);
    puts("\npress any key to stop\n");

    started = times(0);
    sched_start = started;

    while (!key_waiting()) {
        render();
        step();
        frames++;

        if (every && generation % every == 0) {
            report("");
        }
        if (settled() && par[P_RESTART].value) {
            report("settled at ");
            seed_board();
            boards++;
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

        puts("life: ");
        putdec(frames);
        puts(" generations in ");
        putdec(elapsed / HZ);
        puts(" seconds");
        if (elapsed > 0) {
            puts(" (");
            putdec(frames * HZ / elapsed);
            puts(" per second)");
        }
        if (boards > 1) {
            puts(", ");
            putdec(boards);
            puts(" boards");
        }
        putch('\n');
    }
    return 0;
}
