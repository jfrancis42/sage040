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
/*
 * MAP_SHARED. The page every shared mapping of that page of the file
 * gets, with a reference for the caller, or 0; `writable` marks it
 * dirty. vm.c reports each further mapping of it (fork), each one that
 * becomes writable (mprotect) and each one that goes; the last to go
 * writes it back. msync is textcache_sync_page; fsync, and read() and
 * write() and truncate before they act, textcache_sync_file /
 * textcache_before_io; write() and truncate after,
 * textcache_after_change, which reads the shared pages in again.
 */
struct file;
u32  textcache_get_shared(int fd, u32 off, int writable);
void textcache_share_dup(u32 pa);
void textcache_share_dirty(u32 pa);
void textcache_share_release(u32 pa);
int  textcache_sync_page(u32 pa);
int  textcache_sync_file(struct file *f);
void textcache_before_io(struct file *f);
void textcache_after_change(struct file *f);

/* Pages only the cache holds: memory that is free for the asking,
 * which sysinfo reports as bufferram and `free` as cache. */
u32  textcache_idle(void);

/* Give up to `want` of those back to the allocator. How many it did. */
u32  textcache_shrink(u32 want);

#endif
