/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * textcache.c - one copy of a file's read-only pages, however many
 * processes map them.
 *
 * This is the half of shared libraries that is about memory. A program
 * linked against libc.so maps libc's text read-only (ldso/ld.c); without
 * this, each of those mappings would be a private copy read from disk,
 * and "shared" would describe the file and nothing else. With it, the
 * first process to map a page of the file reads it into a page this
 * cache keeps, and every mapping after that -- in the same process or
 * any other -- gets the SAME physical page, reference counted by pmm.
 *
 * ONLY READ-ONLY, PRIVATE MAPPINGS come here (mmap.c decides), and that
 * is what makes sharing a page safe without any copy-on-write machinery
 * in the fault path: nobody can write to it. A process that later asks
 * to write -- mprotect(PROT_WRITE), or ld.so applying a text relocation
 * -- is given its own copy at that moment (vm_protect()), which is
 * copy-on-write done eagerly, at the one place a write can be granted.
 *
 * A page stays cached after its last mapping goes, so the next program
 * to start finds libc already in memory. It is dropped when the file
 * changes (textcache_forget(), which the VFS calls on every write,
 * truncate, unlink and rename), and when the table is full and room is
 * needed, an entry whose page nobody else is using is given up.
 *
 * The key is the file's inode number and the page's offset in it. FAT
 * has no inode numbers; fat16.c makes them from where the directory
 * entry is, which is stable for as long as the file is not moved or
 * deleted -- and moving and deleting are two of the changes that forget.
 */
#include "textcache.h"
#include "vfs.h"
#include "pmm.h"
#include "errno.h"
#include "string.h"

#define TC_ENTRIES   1024       /* 4 MB of text, which is several libcs */
#define TC_BUCKETS   256
#define TC_NONE      0xffff

struct tc_entry {
    u32 ino;
    u32 off;                    /* page-aligned offset in the file */
    u32 pa;                     /* 0: the slot is free             */
    u16 next;                   /* chain within a bucket           */
};

static struct tc_entry entries[TC_ENTRIES];
static u16 buckets[TC_BUCKETS];
static int ready;

static struct tc_stats stats;

static u32 bucket_of(u32 ino, u32 off)
{
    return (ino * 31u + (off >> PAGE_SHIFT)) % TC_BUCKETS;
}

static void init_once(void)
{
    u32 i;

    if (ready) {
        return;
    }
    for (i = 0; i < TC_BUCKETS; i++) {
        buckets[i] = TC_NONE;
    }
    ready = 1;
}

/* Take entry `i` out of its bucket's chain and give up the cache's own
 * reference to the page. Anyone still mapping it keeps it. */
static void drop(u32 i)
{
    struct tc_entry *e = &entries[i];
    u16 *link = &buckets[bucket_of(e->ino, e->off)];

    while (*link != TC_NONE && *link != i) {
        link = &entries[*link].next;
    }
    if (*link == i) {
        *link = e->next;
    }
    pmm_free(e->pa);
    e->pa = 0;
    stats.cached--;
}

static int find(u32 ino, u32 off)
{
    u16 i;

    for (i = buckets[bucket_of(ino, off)]; i != TC_NONE; i = entries[i].next) {
        if (entries[i].ino == ino && entries[i].off == off) {
            return i;
        }
    }
    return -1;
}

/* A free slot, making one by giving up a page only the cache holds.
 * -1 when every cached page is in use somewhere. */
static int free_slot(void)
{
    static u32 hand;
    u32 i, k;

    for (i = 0; i < TC_ENTRIES; i++) {
        if (!entries[i].pa) {
            return (int)i;
        }
    }
    /* Round the table from where the last eviction left off, so one
     * unlucky entry is not the one given up every time. */
    for (k = 0; k < TC_ENTRIES; k++) {
        i = (hand + k) % TC_ENTRIES;
        if (pmm_refcount(entries[i].pa) == 1) {
            hand = i + 1;
            drop(i);
            stats.evicted++;
            return (int)i;
        }
    }
    return -1;
}

/* Read one page of the file, zero past its end, into `pa`. */
static int fill(int fd, u32 off, u32 pa)
{
    s32 saved = fd_lseek(fd, 0, SEEK_CUR);
    s32 got;

    if (saved < 0 || fd_lseek(fd, (s32)off, SEEK_SET) < 0) {
        return -EIO;
    }
    got = fd_read(fd, (void *)pa, PAGE_SIZE);
    fd_lseek(fd, saved, SEEK_SET);
    return got < 0 ? (int)got : 0;
}

u32 textcache_get(int fd, u32 off)
{
    struct stat st;
    u32 pa;
    int i;

    init_once();
    if (off & PAGE_MASK || vfs_fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }

    i = find(st.st_ino, off);
    if (i >= 0 && pmm_ref(entries[i].pa)) {
        stats.hits++;
        return entries[i].pa;
    }

    stats.misses++;
    pa = pmm_alloc();
    if (!pa) {
        return 0;
    }
    if (fill(fd, off, pa) < 0) {
        pmm_free(pa);
        return 0;
    }
    if (i >= 0) {
        return pa;              /* shared by 65536 already: a copy */
    }
    i = free_slot();
    if (i < 0 || !pmm_ref(pa)) {
        return pa;              /* no room to keep it: still a good page */
    }
    entries[i].ino = st.st_ino;
    entries[i].off = off;
    entries[i].pa = pa;
    entries[i].next = buckets[bucket_of(st.st_ino, off)];
    buckets[bucket_of(st.st_ino, off)] = (u16)i;
    stats.cached++;
    return pa;
}

void textcache_forget(u32 ino)
{
    u32 i;

    if (!ready || !stats.cached) {
        return;
    }
    for (i = 0; i < TC_ENTRIES; i++) {
        if (entries[i].pa && entries[i].ino == ino) {
            drop(i);
            stats.forgotten++;
        }
    }
}

void textcache_forget_all(void)
{
    u32 i;

    if (!ready) {
        return;
    }
    for (i = 0; i < TC_ENTRIES; i++) {
        if (entries[i].pa) {
            drop(i);
            stats.forgotten++;
        }
    }
}

void textcache_forget_fd(int fd)
{
    struct stat st;

    if (ready && stats.cached && vfs_fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        textcache_forget(st.st_ino);
    }
}

void textcache_forget_path(const char *path)
{
    struct stat st;

    if (ready && stats.cached && vfs_stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        textcache_forget(st.st_ino);
    }
}

u32 textcache_idle(void)
{
    u32 i, n = 0;

    for (i = 0; ready && i < TC_ENTRIES; i++) {
        if (entries[i].pa && pmm_refcount(entries[i].pa) == 1) {
            n++;
        }
    }
    return n;
}

void textcache_stats(struct tc_stats *out)
{
    *out = stats;
}
