/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * malloctest - exercise malloc, free, realloc and calloc.
 *
 * A handful of fixed checks, then a long randomised workload: thousands
 * of allocations of random sizes, freed and reallocated in random order,
 * every one filled with a pattern that says which slot owns it. The
 * patterns are checked before every free and after every realloc, and
 * malloc_check() walks the whole heap every few hundred operations. A
 * list that has been corrupted, two blocks that overlap, or a realloc
 * that lost data all show up as a wrong line rather than as a crash
 * somewhere else later.
 *
 * `malloctest doublefree` frees one block twice. That must be caught and
 * end the program; the harness checks what it said.
 */
#include "ulib.h"

#define SLOTS   512
#define OPS     40000

static u8 *slot[SLOTS];
static u32 slot_len[SLOTS];

static u32 seed = 12345;

static u32 rnd(void)
{
    /* Numerical Recipes' LCG: plenty for choosing sizes and orders. */
    seed = seed * 1664525UL + 1013904223UL;
    return seed >> 8;
}

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static u8 pattern(int i, u32 off)
{
    return (u8)(i * 31 + off * 7 + 1);
}

static void fill(int i)
{
    u32 k;

    for (k = 0; k < slot_len[i]; k++) {
        slot[i][k] = pattern(i, k);
    }
}

static int intact(int i, u32 len)
{
    u32 k;

    for (k = 0; k < len; k++) {
        if (slot[i][k] != pattern(i, k)) {
            return 0;
        }
    }
    return 1;
}

/* Mostly small, sometimes medium, now and then big enough for mmap. */
static u32 some_size(void)
{
    u32 r = rnd() % 100;

    if (r < 70) {
        return rnd() % 64;
    }
    if (r < 95) {
        return rnd() % 4096;
    }
    if (r < 99) {
        return rnd() % 65536;
    }
    return 131072 + rnd() % 131072;
}

static void fixed_checks(void)
{
    u8 *a, *b, *c, *p;
    u32 i, top;
    int ok;
    struct mallinfo m;

    a = malloc(1);
    b = malloc(1);
    report("malloc returns memory", a && b && a != b);
    report("  aligned to 8 bytes",
           ((u32)a & 7) == 0 && ((u32)b & 7) == 0);
    free(a);
    free(b);

    a = malloc(0);
    b = malloc(0);
    report("malloc(0) returns distinct pointers that can be freed",
           a && b && a != b);
    free(a);
    free(b);
    free(0);
    report("free(NULL) does nothing", 1);

    p = calloc(1000, 4);
    ok = p != 0;
    for (i = 0; ok && i < 4000; i++) {
        ok = p[i] == 0;
    }
    report("calloc returns zeroed memory", ok);
    memset(p, 0xee, 4000);
    free(p);
    p = calloc(1000, 4);
    ok = p != 0;
    for (i = 0; ok && i < 4000; i++) {
        ok = p[i] == 0;
    }
    report("  even when it reuses memory that was dirty", ok);
    free(p);
    report("calloc whose product overflows returns NULL",
           calloc(0x10000, 0x10001) == 0);

    /* More than the machine has, ASKED OF THE MACHINE rather than
     * written down: a flat 200 MB stopped being "more than exists" the
     * day the default RAM became 256 MB. See memtest.c, same change. */
    {
        struct sysinfo si;
        unsigned long huge = 200UL * 1024 * 1024;

        if (sysinfo(&si) == 0 && si.totalram) {
            huge = (unsigned long)si.totalram * 4096UL + 16UL * 1024 * 1024;
        }
        report("malloc of more than exists returns NULL", malloc(huge) == 0);
    }
    report("  and the heap is still sound", malloc_check() == 0);

    /* realloc keeps the contents, and grows in place when it can. */
    a = malloc(100);
    for (i = 0; i < 100; i++) {
        a[i] = (u8)i;
    }
    b = realloc(a, 5000);
    ok = b != 0;
    for (i = 0; ok && i < 100; i++) {
        ok = b[i] == (u8)i;
    }
    report("realloc preserves the contents", ok);
    c = realloc(b, 20000);
    report("realloc grows the last block in place", c == b);
    report("realloc to a smaller size keeps the pointer",
           realloc(c, 10) == c && c[5] == 5);
    report("realloc(NULL, n) is malloc", (a = realloc(0, 10)) != 0);
    report("realloc(p, 0) frees and returns NULL", realloc(a, 0) == 0);
    free(c);

    /* The big ones come from mmap and go back to it. */
    m = mallinfo();
    a = malloc(300000);
    report("a 300 KB block comes from mmap", mallinfo().hblks == m.hblks + 1);
    a[0] = 1;
    a[299999] = 2;
    report("  and both ends of it are usable", a[0] == 1 && a[299999] == 2);
    b = realloc(a, 600000);
    report("  and it can grow", b && b[0] == 1 && b[299999] == 2);
    free(b);
    report("  and free gives it back", mallinfo().hblks == m.hblks);

    /* A big free block at the top of the heap goes back to the system. */
    top = (u32)sbrk(0);
    a = malloc(100000);
    b = malloc(100000);
    c = malloc(100000);
    report("the heap grew for 300 KB of small blocks",
           (u32)sbrk(0) >= top + 300000);
    free(c);
    free(b);
    free(a);
    report("  and freeing them gives most of it back",
           (u32)sbrk(0) < top + 128 * 1024);
    report("  and the heap is still sound", malloc_check() == 0);
}

static void workload(void)
{
    u32 op, checks = 0, check_fail = 0, lost = 0, nulls = 0;
    int i;
    struct mallinfo m;

    for (op = 0; op < OPS; op++) {
        i = (int)(rnd() % SLOTS);

        if (!slot[i]) {
            slot_len[i] = some_size();
            slot[i] = malloc(slot_len[i]);
            if (!slot[i]) {
                nulls++;
                continue;
            }
            fill(i);
        } else if (rnd() % 3 == 0) {
            u32 n = some_size();
            u32 keep = n < slot_len[i] ? n : slot_len[i];
            u8 *q;

            if (!intact(i, slot_len[i])) {
                lost++;
            }
            q = realloc(slot[i], n);
            if (!q && n) {
                nulls++;
                continue;
            }
            slot[i] = q;
            if (q && !intact(i, keep)) {
                lost++;
            }
            slot_len[i] = n;
            if (q) {
                fill(i);
            }
        } else {
            if (!intact(i, slot_len[i])) {
                lost++;
            }
            free(slot[i]);
            slot[i] = 0;
        }

        if (op % 500 == 0) {
            checks++;
            if (malloc_check() < 0) {
                check_fail++;
            }
        }
    }

    report("40000 random operations completed", 1);
    report("  no allocation came back NULL", nulls == 0);
    report("  no block's contents were damaged", lost == 0);
    report("  the heap was sound every time it was checked",
           checks > 0 && check_fail == 0);

    for (i = 0; i < SLOTS; i++) {
        if (slot[i] && !intact(i, slot_len[i])) {
            lost++;
        }
        free(slot[i]);
        slot[i] = 0;
    }
    m = mallinfo();
    report("after freeing everything, nothing is in use",
           m.uordblks == 0 && m.hblks == 0);
    report("  and all the free memory is in one block per segment",
           malloc_check() == 0 && m.ordblks >= 1 && m.ordblks <= 4);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "doublefree") == 0) {
        u8 *p = malloc(40);

        free(p);
        puts("malloctest: freeing the same block twice\n");
        free(p);
        puts("NOT-CAUGHT\n");
        return 0;
    }

    fixed_checks();
    workload();
    puts("malloctest: done\n");
    return 0;
}
