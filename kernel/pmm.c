/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pmm.c - the physical page allocator.
 */
#include "pmm.h"
#include "console.h"
#include "string.h"

/*
 * The bitmap lives at the FRONT OF THE POOL IT DESCRIBES, and the pages
 * it occupies are marked used before anything else can be handed out.
 *
 * That is the usual bootstrap knot -- the allocator has to allocate its
 * own bookkeeping before it can allocate anything -- and it is untied
 * here by not allocating: the size is known from the range, so the
 * bitmap is simply placed at the bottom of that range and the
 * corresponding bits are set by hand.
 *
 * It was a static array in .bss sized for 64 MB, with the reasoning
 * that a 68040 machine with more than that is not the machine this is.
 * That was true of the machine and false of the emulator, which accepts
 * up to 2 GB -- and the failure was silent, because pmm_init() clamped
 * to MAX_PAGES and reported the clamped figure as though it were the
 * memory found. Scaling it costs one page per 128 MB -- and the
 * reference counts below, placed after it, two bytes a page.
 */
static u8 *bitmap;

/*
 * REFERENCES BEYOND THE FIRST, one count per page, placed after the
 * bitmap the same way. Zero for almost every page: an allocated page has
 * one owner and pmm_free() gives it back. A page shared between address
 * spaces -- a shared library's text, from textcache.c -- is counted up
 * by pmm_ref() for each extra holder, and pmm_free() only counts it down
 * until the last one lets go.
 *
 * Counting the EXTRA references, rather than all of them, is what lets
 * every existing caller stay as it is: pmm_alloc() did not have to learn
 * to set a count, and a page nobody shares frees exactly as before.
 */
static u16 *refs;
static u32 base;                /* physical address of page 0 of the pool */
static u32 total;
static u32 used;

static int test_bit(u32 i)
{
    return (bitmap[i >> 3] >> (i & 7)) & 1;
}

static void set_bit(u32 i)
{
    bitmap[i >> 3] |= (u8)(1 << (i & 7));
}

static void clear_bit(u32 i)
{
    bitmap[i >> 3] &= (u8)~(1 << (i & 7));
}

void pmm_init(u32 first, u32 last)
{
    u32 pages, map_bytes, map_pages, i;

    used = 0;

    base = PAGE_ALIGN_UP(first);
    last = PAGE_ALIGN_DOWN(last);

    if (last <= base) {
        total = 0;
        bitmap = 0;
        return;
    }

    pages = (last - base) / PAGE_SIZE;

    /*
     * One bit per page, rounded to whole pages, taken off the front.
     * Written through the identity map: the MMU is not on yet, so the
     * address is physical either way.
     */
    map_bytes = (pages + 7) / 8;
    map_bytes = (map_bytes + 1) & ~1UL;         /* refs[] is u16 */
    map_bytes += pages * sizeof(u16);
    map_pages = (map_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    if (map_pages >= pages) {
        total = 0;
        bitmap = 0;
        return;
    }

    bitmap = (u8 *)base;
    memset(bitmap, 0, map_pages * PAGE_SIZE);
    refs = (u16 *)(base + (((pages + 7) / 8 + 1) & ~1UL));

    total = pages;

    /* The bitmap's own pages are not available. */
    for (i = 0; i < map_pages; i++) {
        set_bit(i);
        used++;
    }
}

u32 pmm_alloc(void)
{
    u32 i;

    for (i = 0; i < total; i++) {
        if (!test_bit(i)) {
            u32 pa = base + i * PAGE_SIZE;

            set_bit(i);
            used++;
            /*
             * Zeroed through the identity map, which is why this is
             * correct both before the MMU is on (addresses are physical)
             * and after (the kernel's map is identity for all of RAM).
             * It would stop being correct the moment the kernel stopped
             * mapping all of memory, and that is the assumption to come
             * back to if this ever reads oddly.
             */
            memset((void *)pa, 0, PAGE_SIZE);
            return pa;
        }
    }
    return 0;
}

void pmm_free(u32 pa)
{
    u32 i;

    if (pa < base) {
        return;
    }
    i = (pa - base) / PAGE_SIZE;
    if (i < total && test_bit(i) && refs[i]) {
        refs[i]--;              /* somebody else still has it */
        return;
    }
    if (i >= total || !test_bit(i)) {
        /*
         * Freeing something twice, or something that was never ours.
         * Ignored rather than trusted: the alternative is handing the
         * same page out twice, and two programs sharing a page neither
         * asked to share is not a failure anybody would diagnose
         * quickly.
         */
        return;
    }
    clear_bit(i);
    used--;
}

u32 pmm_alloc_pages(u32 n)
{
    u32 i, j;

    if (n == 0) {
        return 0;
    }
    if (n == 1) {
        return pmm_alloc();
    }

    for (i = 0; i + n <= total; i++) {
        for (j = 0; j < n; j++) {
            if (test_bit(i + j)) {
                break;
            }
        }
        if (j == n) {
            u32 pa = base + i * PAGE_SIZE;

            for (j = 0; j < n; j++) {
                set_bit(i + j);
            }
            used += n;
            memset((void *)pa, 0, n * PAGE_SIZE);
            return pa;
        }
        /* Skip the run that just failed rather than retrying inside it. */
        i += j;
    }
    return 0;
}

void pmm_free_pages(u32 pa, u32 n)
{
    while (n-- > 0) {
        pmm_free(pa);
        pa += PAGE_SIZE;
    }
}

int pmm_ref(u32 pa)
{
    u32 i;

    if (pa < base) {
        return 0;
    }
    i = (pa - base) / PAGE_SIZE;
    if (i >= total || !test_bit(i) || refs[i] == 0xffff) {
        return 0;               /* not a page, or shared by 65536 already */
    }
    refs[i]++;
    return 1;
}

u32 pmm_refcount(u32 pa)
{
    u32 i;

    if (pa < base) {
        return 0;
    }
    i = (pa - base) / PAGE_SIZE;
    if (i >= total || !test_bit(i)) {
        return 0;
    }
    return 1 + (u32)refs[i];
}

u32 pmm_total(void)
{
    return total;
}

u32 pmm_available(void)
{
    return total - used;
}
