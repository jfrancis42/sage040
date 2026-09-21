/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pmm.c - the physical page allocator.
 */
#include "pmm.h"
#include "console.h"
#include "string.h"

/*
 * Enough bitmap for 64 MB of RAM at 4 KB a page.
 *
 * Static rather than carved out of the memory it describes, which is the
 * usual bootstrap knot: the allocator would have to allocate its own
 * bookkeeping before it could allocate anything. Two kilobytes of .bss
 * unties it, and a 68040 machine with more than 64 MB is not the machine
 * this is.
 */
#define MAX_PAGES   (64UL * 1024 * 1024 / PAGE_SIZE)

static u8  bitmap[MAX_PAGES / 8];
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
    u32 pages;

    memset(bitmap, 0, sizeof(bitmap));
    used = 0;

    base = PAGE_ALIGN_UP(first);
    last = PAGE_ALIGN_DOWN(last);

    if (last <= base) {
        total = 0;
        return;
    }

    pages = (last - base) / PAGE_SIZE;
    if (pages > MAX_PAGES) {
        pages = MAX_PAGES;
    }
    total = pages;
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

u32 pmm_total(void)
{
    return total;
}

u32 pmm_available(void)
{
    return total - used;
}
