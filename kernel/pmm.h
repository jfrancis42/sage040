/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pmm.h - physical memory, a page at a time.
 *
 * The bottom of the memory system. Everything above it -- page tables,
 * a program's image, a program's stack -- is built out of pages handed
 * out here, and none of it cares where in RAM a page came from.
 *
 * A bitmap, not a free list. A free list would be smaller code and would
 * mean writing the link into the page itself, which is fine right up
 * until something is looking for the bug where a freed page is still
 * being written to; a bitmap keeps the bookkeeping out of the pages so
 * that a use-after-free corrupts data rather than the allocator. On a
 * machine with a few thousand pages the size difference is a few hundred
 * bytes.
 *
 * There is no allocator for anything smaller. The kernel's own data is
 * static and its stack is fixed, so a page is the only unit anything
 * asks for -- and a kmalloc that nothing needed would be a thing to
 * maintain for no return.
 */
#ifndef PMM_H
#define PMM_H

#include "kernel.h"

#define PAGE_SIZE   4096UL
#define PAGE_SHIFT  12
#define PAGE_MASK   (PAGE_SIZE - 1)

#define PAGE_ALIGN_DOWN(x)  ((u32)(x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)    PAGE_ALIGN_DOWN((u32)(x) + PAGE_MASK)

/*
 * Take [first, last) as the free pool.
 *
 * main.c passes the end of the kernel and the bottom of its stack. The
 * kernel's own image is never in the pool, which is the only reason it
 * is safe to hand a page to a program that will write anything it likes
 * to it.
 */
void pmm_init(u32 first, u32 last);

/*
 * One page, zeroed, or 0 if there are none left.
 *
 * Zeroed because every caller wants it that way -- a page table with
 * rubbish in it is a wild pointer the hardware follows, and a program's
 * fresh page should not contain the last program's data. Doing it here
 * rather than at each call site means it cannot be forgotten at one of
 * them.
 */
u32  pmm_alloc(void);
void pmm_free(u32 pa);

/*
 * `n` pages that are next to each other, or 0.
 *
 * Needed because a stack has to be contiguous: it grows downward
 * through addresses and does not consult anything on the way. Single
 * pages can come from anywhere, but the moment something spans more than
 * one page it has to actually span.
 */
u32  pmm_alloc_pages(u32 n);
void pmm_free_pages(u32 pa, u32 n);

u32  pmm_total(void);           /* pages in the pool */
u32  pmm_available(void);       /* pages not handed out */

#endif /* PMM_H */
