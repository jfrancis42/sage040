/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * swap.h - the swap file. See swap.c.
 */
#ifndef SWAP_H
#define SWAP_H

#include "kernel.h"

#define SWAP_NONE   0xffffffffUL

struct swapstats {
    u32 slots;                  /* usable pages in the swap file     */
    u32 used;                   /* of which holding a page now       */
    u32 pageouts, pageins;      /* since swapon                       */
};

/* swapon: the number of usable slots, or -errno. */
int  swap_on(const char *path);
void swap_release(void);
int  swap_is_on(void);
int  swap_matches(const char *path);   /* is this path the swap file?  */
int  swap_holds(u32 ino);              /* is this inode the swap file? */

/* A free slot with one holder, or SWAP_NONE. */
u32  swap_alloc(void);
int  swap_ref(u32 slot);               /* another holder; -1 if not    */
void swap_free(u32 slot);              /* one holder fewer             */
u32  swap_refcount(u32 slot);

/* A write to the slot is under way; a fault on it waits (swap.c). */
void swap_set_busy(u32 slot, int busy);
void swap_wait_idle(u32 slot);

/* One page, straight to or from the disk. */
int  swap_write(u32 slot, const void *page);
int  swap_read(u32 slot, void *page);

void swap_stats(struct swapstats *out);

#endif
