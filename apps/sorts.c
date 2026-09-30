/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sorts.c - sorting algorithms, watched.
 *
 *     sage$ sorts [options]      (sorts -h lists them)
 *
 * A shuffled row of bars, one per value, sorted in front of you: every
 * write to the array redraws that bar, lit up for a moment, so the
 * shape each algorithm gives the unsorted part as it works is visible --
 * bubble sort's slow tide, insertion sort's growing ramp, quicksort's
 * partitions, heapsort's heap, merge sort's runs, shellsort's gaps. It
 * goes through them all in turn, each on a fresh shuffle, and prints
 * how many comparisons and writes each took.
 *
 * Drawn straight onto the screen (single buffered), two blitter
 * rectangles per write -- the bar, and the empty space above it.
 *
 * Keys: + and - change the speed, space pauses, s skips to the next
 * algorithm, q stops.
 */
#include "gfx.h"
#include "malloc.h"

#define C_BG    GFX_PAL_FREE
#define C_BAR   (GFX_PAL_FREE + 1)
#define C_HOT   (GFX_PAL_FREE + 2)

enum { P_WIDTH, P_SPEED, P_ALGO, P_NPARAM };

static struct gfx_opt par[P_NPARAM] = {
    [P_WIDTH] = GFX_OPT_NUM('z', "bar width", 4, 1, 64, "px"),
    [P_SPEED] = GFX_OPT_NUM('S', "writes shown between pauses", 8, 1, 100000,
                            ""),
    [P_ALGO]  = GFX_OPT_STR('a', "algorithm, or all", "all"),
};

static struct gfx g;
static int n, bw, hmax;
static int *a;
static u32 compares, writes, since;
static int hot = -1;            /* the bar last lit                 */
static int stop, skip;          /* q, and s: finish without drawing */

static void rect(int x, int y, int w, int h, u32 colour)
{
    struct fb_rect r;

    if (w <= 0 || h <= 0) {
        return;
    }
    r.x = x; r.y = y; r.w = w; r.h = h;
    r.colour = colour;
    r.filled = 1;
    ioctl(g.fd, FBIO_RECT, (u32)&r);
}

static void bar(int i, u32 colour)
{
    int top = (int)g.info.height - a[i] * hmax / n;

    rect(i * bw, 0, bw, top, C_BG);
    rect(i * bw, top, bw, (int)g.info.height - top, colour);
}

/* Between writes: keys, and a pause every so often so it can be seen. */
static void pace(void)
{
    int k = gfx_key();

    if (gfx_quit_key(k)) {
        stop = 1;
    } else if (k == 's') {
        skip = 1;
    } else if (k == '+' || k == '=') {
        par[P_SPEED].value *= 2;
    } else if (k == '-') {
        if (par[P_SPEED].value > 1) par[P_SPEED].value /= 2;
    } else if (k == ' ') {
        while (!gfx_quit_key(k = gfx_key_wait()) && k != ' ') {
        }
        if (gfx_quit_key(k)) {
            stop = 1;
        }
    }
    if (++since >= par[P_SPEED].value) {
        since = 0;
        msleep(10);
    }
}

static int less(int i, int j)
{
    compares++;
    return a[i] < a[j];
}

static int less_v(int v, int j)
{
    compares++;
    return v < a[j];
}

static void put(int i, int v)
{
    a[i] = v;
    writes++;
    if (stop || skip) {
        return;                         /* finish at full speed */
    }
    if (hot >= 0 && hot != i) {
        bar(hot, C_BAR);
    }
    bar(i, C_HOT);
    hot = i;
    pace();
}

static void swap(int i, int j)
{
    int t = a[i];

    put(i, a[j]);
    put(j, t);
}

/* ---------------------------------------------------------------- */
/* The algorithms                                                    */
/* ---------------------------------------------------------------- */

static void bubble(void)
{
    int i, j, moved = 1;

    for (i = n - 1; i > 0 && moved; i--) {
        moved = 0;
        for (j = 0; j < i; j++) {
            if (less(j + 1, j)) {
                swap(j, j + 1);
                moved = 1;
            }
        }
    }
}

static void insertion(void)
{
    int i, j;

    for (i = 1; i < n; i++) {
        int v = a[i];

        for (j = i - 1; j >= 0 && less_v(v, j); j--) {
            put(j + 1, a[j]);
        }
        put(j + 1, v);
    }
}

static void selection(void)
{
    int i, j, m;

    for (i = 0; i < n - 1; i++) {
        for (m = i, j = i + 1; j < n; j++) {
            if (less(j, m)) {
                m = j;
            }
        }
        if (m != i) {
            swap(i, m);
        }
    }
}

static void shell(void)
{
    int gap, i, j;

    for (gap = 1; gap < n / 3; gap = gap * 3 + 1) {
    }
    for (; gap > 0; gap /= 3) {
        for (i = gap; i < n; i++) {
            int v = a[i];

            for (j = i; j >= gap && less_v(v, j - gap); j -= gap) {
                put(j, a[j - gap]);
            }
            put(j, v);
        }
    }
}

static void quick(int lo, int hi)
{
    while (lo < hi) {
        int p = lo + (hi - lo) / 2, i = lo, j;

        swap(p, hi);                    /* the middle as pivot */
        for (j = lo; j < hi; j++) {
            if (less(j, hi)) {
                swap(i, j);
                i++;
            }
        }
        swap(i, hi);
        /* The smaller side by recursion, the larger by the loop, so the
         * stack stays shallow whatever the input. */
        if (i - lo < hi - i) {
            quick(lo, i - 1);
            lo = i + 1;
        } else {
            quick(i + 1, hi);
            hi = i - 1;
        }
    }
}

static void quicksort(void)
{
    quick(0, n - 1);
}

static void sift(int root, int end)
{
    for (;;) {
        int c = root * 2 + 1;

        if (c > end) {
            return;
        }
        if (c + 1 <= end && less(c, c + 1)) {
            c++;
        }
        if (!less(root, c)) {
            return;
        }
        swap(root, c);
        root = c;
    }
}

static void heapsort(void)
{
    int i;

    for (i = n / 2 - 1; i >= 0; i--) {
        sift(i, n - 1);
    }
    for (i = n - 1; i > 0; i--) {
        swap(0, i);
        sift(0, i - 1);
    }
}

static int *tmp;

static void merge(int lo, int mid, int hi)
{
    int i = lo, j = mid, k = 0;

    while (i < mid && j < hi) {
        compares++;
        tmp[k++] = a[j] < a[i] ? a[j++] : a[i++];
    }
    while (i < mid) {
        tmp[k++] = a[i++];
    }
    while (j < hi) {
        tmp[k++] = a[j++];
    }
    for (i = 0; i < k; i++) {
        put(lo + i, tmp[i]);
    }
}

static void mergesort(void)
{
    int width, lo;

    for (width = 1; width < n; width *= 2) {
        for (lo = 0; lo < n - width; lo += 2 * width) {
            int hi = lo + 2 * width;

            merge(lo, lo + width, hi < n ? hi : n);
        }
    }
}

static const struct {
    const char *name;
    void (*run)(void);
} algos[] = {
    { "bubble", bubble }, { "insertion", insertion },
    { "selection", selection }, { "shell", shell }, { "quick", quicksort },
    { "heap", heapsort }, { "merge", mergesort },
};
#define NALGOS  (sizeof(algos) / sizeof(algos[0]))

static void shuffle(void)
{
    int i;

    for (i = 0; i < n; i++) {
        a[i] = i + 1;
    }
    for (i = n - 1; i > 0; i--) {
        int j = (int)(gfx_rand() % (u32)(i + 1)), t = a[i];

        a[i] = a[j];
        a[j] = t;
    }
    rect(0, 0, (int)g.info.width, (int)g.info.height, C_BG);
    for (i = 0; i < n; i++) {
        bar(i, C_BAR);
    }
}

static int sorted(void)
{
    int i;

    for (i = 1; i < n; i++) {
        if (a[i - 1] > a[i]) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    u32 i, first = 0, last = NALGOS - 1;
    int r, bad = 0;

    r = gfx_options(argc, argv, "sorts", par, P_NPARAM, 0);
    if (r) {
        return r > 0 ? 0 : 2;
    }
    if (strcmp(par[P_ALGO].str, "all") != 0) {
        for (i = 0; i < NALGOS && strcmp(par[P_ALGO].str, algos[i].name);
             i++) {
        }
        if (i == NALGOS) {
            eputs("sorts: the algorithms are bubble insertion selection "
                  "shell quick heap merge, or all\n");
            return 2;
        }
        first = last = i;
    }
    if (gfx_open(&g, "sorts", 0) < 0) {
        return 1;
    }
    ioctl(g.fd, FBIO_DOUBLE, 0);        /* drawn where it is shown */
    ioctl(g.fd, FBIO_GETINFO, (u32)&g.info);
    bw = (int)par[P_WIDTH].value;
    n = (int)g.info.width / bw;
    hmax = (int)g.info.height - 8;
    a = malloc((u32)n * sizeof(int));
    tmp = malloc((u32)n * sizeof(int));
    if (!a || !tmp) {
        gfx_close(&g);
        eputs("sorts: not enough memory\n");
        return 1;
    }
    gfx_seed();
    gfx_colour(&g, C_BG, 0x000000UL);
    gfx_colour(&g, C_BAR, 0x4080ffUL);
    gfx_colour(&g, C_HOT, 0xffffffUL);
    puts("sorts: + - speed, s skips, space pauses, q stops\n");

    for (i = first; !stop; ) {
        shuffle();
        compares = writes = 0;
        hot = -1;
        skip = 0;
        algos[i].run();
        if (!sorted()) {
            bad = 1;
        }
        if (hot >= 0) {
            bar(hot, C_BAR);
        }
        puts("sorts: ");
        puts(algos[i].name);
        puts(", ");
        putdec((u32)n);
        puts(" bars: ");
        putdec(compares);
        puts(" comparisons, ");
        putdec(writes);
        puts(sorted() ? " writes, sorted\n" : " writes, NOT SORTED\n");
        if (!stop && !skip) {
            msleep(1500);
        }
        i = i == last ? first : i + 1;
    }
    gfx_close(&g);
    return bad;
}
