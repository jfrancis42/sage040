/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * textcache.h - shared read-only file pages. See textcache.c.
 */
#ifndef TEXTCACHE_H
#define TEXTCACHE_H

#include "kernel.h"

struct tc_stats {
    u32 cached;                 /* pages held now                      */
    u32 hits;                   /* mappings given a page already held  */
    u32 misses;                 /* ... that had to read it             */
    u32 evicted;                /* given up to make room               */
    u32 forgotten;              /* given up because the file changed   */
};

/*
 * The page at page-aligned offset `off` of the regular file open on
 * `fd`, with a reference taken for the caller -- who gives it back with
 * pmm_free(), usually by way of vm_unmap() or vm_destroy(). The same
 * physical page for every caller, while the cache holds it. 0 when the
 * page cannot be had at all.
 */
u32  textcache_get(int fd, u32 off);

/* The file changed, or is going: its pages are not its contents any
 * more. By inode number, by an open descriptor, or by path. */
void textcache_forget(u32 ino);
void textcache_forget_fd(int fd);
void textcache_forget_path(const char *path);
void textcache_forget_all(void);

void textcache_stats(struct tc_stats *out);

/* Pages only the cache holds: memory that is free for the asking,
 * which sysinfo reports as bufferram and `free` as cache. */
u32  textcache_idle(void);

#endif
