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
 *
 * MAP_SHARED OF A FILE comes here too, and is the other half of what the
 * cache is for: every process mapping page N of the file is given the
 * same physical page, so a store by one is seen by all at once -- which
 * is what a shared mapping is -- and the page goes back to the file when
 * it is msync'd, when the last shared mapping of it goes, and on fsync.
 * Such a page is never forgotten or given up while it is mapped shared
 * (`shared` counts the mappings; vm.c marks their descriptors
 * DESC_SW_SHARED and tells this file when one is copied by fork or
 * released), and the entry holds a reference to an open file of it, so
 * the file can be written back even after every descriptor is closed
 * and every name unlinked, as POSIX has it.
 *
 * WHEN IS IT DIRTY. The 68040 sets a modified bit in the descriptor of
 * a page written through it, but a shared page has one descriptor per
 * mapping and nothing here can find them all from the page. So a page
 * counts as dirty from the moment it is mapped writable until its last
 * shared mapping goes, and is written back every time it is asked --
 * more writing than needed, never less. (The deferred MMU item in the
 * list, using the M bit, is what would make it exact.)
 *
 * AND read() AND write() AGREE WITH IT. read() of a file with dirty
 * shared pages writes them back first, so it reads what the mappings
 * hold; write() writes them back first and reads the pages in again
 * after, so the mappings see what was written; a truncate zeroes what
 * is past the new end. Those are the three ways a file's contents
 * change or are looked at other than through a mapping.
 */
#include "textcache.h"
#include "vfs.h"
#include "pmm.h"
#include "errno.h"
#include "string.h"
#include "dev.h"

#define TC_ENTRIES   1024       /* 4 MB of text, which is several libcs */
#define TC_BUCKETS   256
#define TC_NONE      0xffff

struct tc_entry {
    u32 ino;
    u32 off;                    /* page-aligned offset in the file */
    u32 pa;                     /* 0: the slot is free             */
    u16 next;                   /* chain within a bucket           */
    u16 pnext;                  /* chain by physical page          */
    u16 shared;                 /* MAP_SHARED mappings of the page */
    u8  dirty;                  /* mapped writable since written back */
    struct file *file;          /* held while shared: written back through */
};

static struct tc_entry entries[TC_ENTRIES];
static u16 buckets[TC_BUCKETS];
static u16 pbuckets[TC_BUCKETS];    /* by physical page: what vm.c knows */
static u32 nshared;                 /* entries with shared > 0          */
static int ready;

static struct tc_stats stats;

static u32 bucket_of(u32 ino, u32 off)
{
    return (ino * 31u + (off >> PAGE_SHIFT)) % TC_BUCKETS;
}

static u32 pbucket_of(u32 pa)
{
    return (pa >> PAGE_SHIFT) % TC_BUCKETS;
}

static void init_once(void)
{
    u32 i;

    if (ready) {
        return;
    }
    for (i = 0; i < TC_BUCKETS; i++) {
        buckets[i] = TC_NONE;
        pbuckets[i] = TC_NONE;
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
    link = &pbuckets[pbucket_of(e->pa)];
    while (*link != TC_NONE && *link != i) {
        link = &entries[*link].pnext;
    }
    if (*link == i) {
        *link = e->pnext;
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

static int find_pa(u32 pa)
{
    u16 i;

    for (i = pbuckets[pbucket_of(pa)]; i != TC_NONE; i = entries[i].pnext) {
        if (entries[i].pa == pa) {
            return i;
        }
    }
    return -1;
}

/* Read one page of the file, zero past its end, into `pa`. Through the
 * open file without moving its position: a pread. */
static int fill_file(struct file *f, u32 off, u32 pa)
{
    s32 got = vfs_file_pread(f, off, (void *)pa, PAGE_SIZE);

    if (got < 0) {
        return (int)got;
    }
    memset((u8 *)pa + got, 0, PAGE_SIZE - (u32)got);
    return 0;
}

static int fill(int fd, u32 off, u32 pa)
{
    struct file *f = fd_get(fd);

    return f ? fill_file(f, off, pa) : -EBADF;
}

/* Keep a page already filled, as entry for (ino, off). -1 if no room. */
static int insert(u32 ino, u32 off, u32 pa)
{
    int i = free_slot();

    if (i < 0 || !pmm_ref(pa)) {
        return -1;
    }
    entries[i].ino = ino;
    entries[i].off = off;
    entries[i].pa = pa;
    entries[i].shared = 0;
    entries[i].dirty = 0;
    entries[i].file = 0;
    entries[i].next = buckets[bucket_of(ino, off)];
    buckets[bucket_of(ino, off)] = (u16)i;
    entries[i].pnext = pbuckets[pbucket_of(pa)];
    pbuckets[pbucket_of(pa)] = (u16)i;
    stats.cached++;
    return i;
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

    i = find(vfs_file_key(&st), off);
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
    insert(vfs_file_key(&st), off, pa); /* no room to keep it: still a good page */
    return pa;
}

/* --- shared mappings ------------------------------------------------ */

/*
 * Write one entry's page back to its file: as much of it as the file
 * holds, since a page past the end is not the file's and a mapping may
 * not make a file longer.
 */
static int write_back(struct tc_entry *e)
{
    struct stat st;
    u32 n;
    s32 w;

    if (!e->file || vfs_file_stat(e->file, &st) < 0 || e->off >= st.st_size) {
        return 0;
    }
    n = st.st_size - e->off;
    if (n > PAGE_SIZE) {
        n = PAGE_SIZE;
    }
    w = vfs_file_pwrite(e->file, e->off, (void *)e->pa, n);
    return w < 0 ? (int)w : 0;
}

u32 textcache_get_shared(int fd, u32 off, int writable)
{
    struct file *f = fd_get(fd);
    struct stat st;
    u32 pa;
    int i;

    init_once();
    if (!f || off & PAGE_MASK || vfs_file_stat(f, &st) < 0 ||
        !S_ISREG(st.st_mode)) {
        return 0;
    }
    i = find(vfs_file_key(&st), off);
    if (i < 0) {
        pa = pmm_alloc();
        if (!pa) {
            return 0;
        }
        if (fill_file(f, off, pa) < 0 || (i = insert(vfs_file_key(&st), off, pa)) < 0) {
            /* A shared page has to be THE page: one nobody else can
             * find is no use to a second process mapping the file. */
            pmm_free(pa);
            return 0;
        }
        pmm_free(pa);           /* the cache's reference is enough */
        stats.misses++;
    } else {
        stats.hits++;
    }
    if (!pmm_ref(entries[i].pa)) {
        return 0;
    }
    if (entries[i].shared++ == 0) {
        file_get(f);
        entries[i].file = f;
        nshared++;
    }
    if (writable) {
        entries[i].dirty = 1;
    }
    return entries[i].pa;
}

void textcache_share_dup(u32 pa)
{
    int i = ready ? find_pa(pa) : -1;

    if (i >= 0 && entries[i].shared) {
        entries[i].shared++;
    }
}

void textcache_share_dirty(u32 pa)
{
    int i = ready ? find_pa(pa) : -1;

    if (i >= 0 && entries[i].shared) {
        entries[i].dirty = 1;
    }
}

/*
 * A shared mapping of the page is going. The last one writes the page
 * back and lets go of the file. Called BEFORE the mapping's own
 * reference to the page is given up, so the page is still there to
 * write -- though the cache's would keep it anyway.
 */
void textcache_share_release(u32 pa)
{
    int i = ready ? find_pa(pa) : -1;
    struct tc_entry *e;

    if (i < 0 || !entries[i].shared) {
        return;
    }
    e = &entries[i];
    if (--e->shared == 0) {
        struct file *f = e->file;

        if (e->dirty) {
            write_back(e);
        }
        e->dirty = 0;
        e->file = 0;
        nshared--;
        file_put(f);
    }
}

int textcache_sync_page(u32 pa)
{
    int i = ready ? find_pa(pa) : -1;

    if (i < 0 || !entries[i].shared || !entries[i].dirty) {
        return 0;
    }
    return write_back(&entries[i]);
}

/* Every dirty shared page of the inode, back to the file. */
static int sync_ino(u32 ino)
{
    u32 i;
    int err = 0;

    for (i = 0; i < TC_ENTRIES; i++) {
        struct tc_entry *e = &entries[i];

        if (e->pa && e->ino == ino && e->shared && e->dirty) {
            int r = write_back(e);

            if (r < 0 && !err) {
                err = r;
            }
        }
    }
    return err;
}

static int ino_of(struct file *f, u32 *ino)
{
    struct stat st;

    if (!f || vfs_file_stat(f, &st) < 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    *ino = vfs_file_key(&st);
    return 0;
}

int textcache_sync_file(struct file *f)
{
    u32 ino;

    if (!ready || !nshared || ino_of(f, &ino) < 0) {
        return 0;
    }
    return sync_ino(ino);
}

void textcache_before_io(struct file *f)
{
    textcache_sync_file(f);
}

/*
 * After write() or a truncate: the pages that are mapped shared read
 * the file again, so every mapping sees the new contents -- they were
 * written back first, by textcache_before_io, so nothing a mapping put
 * there is lost -- and the rest are forgotten, as they always were.
 */
void textcache_after_change(struct file *f)
{
    u32 ino, i;

    if (!ready || !stats.cached || ino_of(f, &ino) < 0) {
        return;
    }
    for (i = 0; i < TC_ENTRIES; i++) {
        struct tc_entry *e = &entries[i];

        if (!e->pa || e->ino != ino) {
            continue;
        }
        if (e->shared) {
            fill_file(f, e->off, e->pa);
        } else {
            drop(i);
            stats.forgotten++;
        }
    }
}

void textcache_forget(u32 ino)
{
    u32 i;

    if (!ready || !stats.cached) {
        return;
    }
    /* Not a page mapped shared: the mappings hold the file open, and
     * the page is still that file's however it is renamed or unlinked. */
    for (i = 0; i < TC_ENTRIES; i++) {
        if (entries[i].pa && entries[i].ino == ino && !entries[i].shared) {
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
        if (entries[i].pa && !entries[i].shared) {
            drop(i);
            stats.forgotten++;
        }
    }
}

void textcache_forget_fd(int fd)
{
    struct stat st;

    if (ready && stats.cached && vfs_fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        textcache_forget(vfs_file_key(&st));
    }
}

void textcache_forget_path(const char *path)
{
    struct stat st;

    if (ready && stats.cached && vfs_stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        textcache_forget(vfs_file_key(&st));
    }
}

u32 textcache_shrink(u32 want)
{
    u32 i, n = 0;

    for (i = 0; ready && i < TC_ENTRIES && n < want; i++) {
        if (entries[i].pa && pmm_refcount(entries[i].pa) == 1) {
            drop(i);
            stats.evicted++;
            n++;
        }
    }
    return n;
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
