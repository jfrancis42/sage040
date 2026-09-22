/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * swap.c - the swap file.
 *
 * swapon(path) makes a regular file the place pages go when memory runs
 * out. The file is not read or written through the filesystem after
 * that: at swapon its clusters are turned into disk addresses, a page
 * at a time (the filesystem's bmap), and from then on a page goes to or
 * comes from the disk directly. That is what Linux does with a swap
 * file, and for the same reason -- a page is evicted when memory is
 * short, which can be in the middle of anything, including the
 * filesystem allocating something, and the filesystem must not be
 * re-entered from underneath itself.
 *
 * So the file must not change while it is in use. It is held open, and
 * the VFS refuses to write to, truncate, rename or delete it (vfs.c asks
 * swap_holds()); swapoff lets it go.
 *
 * A page of the file is a SLOT. A page of a program that is swapped out
 * is a descriptor naming its slot (vm.c); a slot has a reference count,
 * because fork() copies the descriptor, and both processes' copies name
 * the same slot until one of them brings it back in.
 *
 * A 4 KB slot must be eight CONSECUTIVE sectors: a slot that would span
 * two clusters that are not next to each other on the disk is simply
 * not used. On a volume with 4 KB clusters or larger that never
 * happens; with smaller ones it happens where the file is fragmented,
 * and swapon reports how many slots it could use.
 */
#include "swap.h"
#include "vfs.h"
#include "dev.h"
#include "pmm.h"
#include "wait.h"
#include "errno.h"
#include "string.h"

#define SECTORS_PER_PAGE  (PAGE_SIZE / 512)
#define SLOT_BAD          0xffffffffUL

static struct {
    int         on;
    struct file *file;          /* held, so it stays what it was      */
    u32         ino;
    struct blockdev *dev;
    u32         nslots;
    u32         *lba;           /* per slot: its first sector, or BAD */
    u8          *refs;          /* per slot: holders; 0 is free       */
    u8          *busy;          /* per slot: a write to it is under way */
    u32         table_pages;    /* what lba[] and refs[] came from    */
    u32         table_pa;
    u32         hint;           /* no free slot below this            */
    struct swapstats st;
} sw;

/*
 * BUSY: a page being written out is off its owner's map before the
 * write starts -- it has to be, since the write sleeps and the owner
 * could otherwise change it half way through, or another task's reclaim
 * choose it again -- so its descriptor already names this slot. A fault
 * on it must wait for the write to finish before reading the slot back.
 */
static struct waitq busy_wait;

void swap_set_busy(u32 slot, int busy)
{
    if (!sw.on || slot >= sw.nslots) {
        return;
    }
    sw.busy[slot] = (u8)(busy != 0);
    if (!busy) {
        wake_all(&busy_wait);
    }
}

void swap_wait_idle(u32 slot)
{
    while (sw.on && slot < sw.nslots && sw.busy[slot]) {
        sleep_on_timeout(&busy_wait, 100);
    }
}

int swap_holds(u32 ino)
{
    return sw.on && ino == sw.ino;
}

int swap_on(const char *path)
{
    struct stat st;
    u32 slots, bytes, pages, pa, i, good = 0;
    int fd, err;

    if (sw.on) {
        return -EBUSY;              /* one swap file, as a first version */
    }
    fd = fd_open(path, O_RDWR);
    if (fd < 0) {
        return fd;
    }
    err = vfs_fstat(fd, &st);
    if (err < 0 || !S_ISREG(st.st_mode)) {
        fd_close(fd);
        return err < 0 ? err : -EINVAL;
    }
    slots = st.st_size / PAGE_SIZE;
    if (slots < 1 || slots > 0x000fffffUL) {
        fd_close(fd);
        return -EINVAL;             /* the descriptor has 20 bits for it */
    }

    bytes = slots * (sizeof(u32) + 2 * sizeof(u8));
    pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    pa = pmm_alloc_pages(pages);
    if (!pa) {
        fd_close(fd);
        return -ENOMEM;
    }
    sw.lba = (u32 *)pa;
    sw.refs = (u8 *)(pa + slots * sizeof(u32));
    sw.busy = sw.refs + slots;

    for (i = 0; i < slots; i++) {
        u32 first, lba, k;

        sw.lba[i] = SLOT_BAD;
        if (vfs_bmap(fd, i * PAGE_SIZE, &first, &sw.dev) < 0) {
            continue;
        }
        for (k = 1; k < SECTORS_PER_PAGE; k++) {
            if (vfs_bmap(fd, i * PAGE_SIZE + k * 512, &lba, &sw.dev) < 0 ||
                lba != first + k) {
                break;
            }
        }
        if (k == SECTORS_PER_PAGE) {
            sw.lba[i] = first;
            good++;
        }
    }
    if (good == 0 || !sw.dev) {
        pmm_free_pages(pa, pages);
        fd_close(fd);
        return -EINVAL;
    }

    /* Held by the kernel, not by whoever called: take the open file out
     * of the caller's descriptor table without closing it. */
    sw.file = fd_get(fd);
    file_get(sw.file);
    fd_close(fd);

    sw.ino = st.st_ino;
    sw.nslots = slots;
    sw.table_pages = pages;
    sw.table_pa = pa;
    sw.hint = 0;
    memset(&sw.st, 0, sizeof(sw.st));
    sw.st.slots = good;
    sw.on = 1;
    return (int)good;
}

/* Called by swapoff once no page is out any more (vm_swapoff). */
void swap_release(void)
{
    if (!sw.on) {
        return;
    }
    file_put(sw.file);
    pmm_free_pages(sw.table_pa, sw.table_pages);
    sw.on = 0;
    sw.file = 0;
    sw.ino = 0;
    sw.nslots = 0;
    sw.st.slots = 0;
    sw.st.used = 0;
}

int swap_is_on(void)
{
    return sw.on;
}

int swap_matches(const char *path)
{
    struct stat st;

    return sw.on && vfs_stat(path, &st) == 0 && st.st_ino == sw.ino;
}

/*
 * FIRST FIT, from the start of the file: a slot given up is the next one
 * used. That keeps what is in use near the front -- and it is also what
 * makes a slot two processes share (after a fork) dangerous to free early,
 * so the test that checks the sharing can see it (pagetest forkswap).
 */
u32 swap_alloc(void)
{
    u32 i;

    if (!sw.on) {
        return SWAP_NONE;
    }
    for (i = sw.hint; i < sw.nslots; i++) {
        if (sw.lba[i] != SLOT_BAD && sw.refs[i] == 0) {
            sw.refs[i] = 1;
            sw.hint = i + 1;
            sw.st.used++;
            return i;
        }
    }
    return SWAP_NONE;
}

int swap_ref(u32 slot)
{
    if (!sw.on || slot >= sw.nslots || sw.refs[slot] == 0 ||
        sw.refs[slot] == 0xff) {
        return -1;
    }
    sw.refs[slot]++;
    return 0;
}

void swap_free(u32 slot)
{
    if (!sw.on || slot >= sw.nslots || sw.refs[slot] == 0) {
        return;
    }
    if (--sw.refs[slot] == 0) {
        sw.st.used--;
        if (slot < sw.hint) {
            sw.hint = slot;             /* the lowest free slot, always */
        }
    }
}

u32 swap_refcount(u32 slot)
{
    return (sw.on && slot < sw.nslots) ? sw.refs[slot] : 0;
}

int swap_write(u32 slot, const void *page)
{
    if (!sw.on || slot >= sw.nslots || sw.lba[slot] == SLOT_BAD) {
        return -EINVAL;
    }
    sw.st.pageouts++;
    return sw.dev->write(sw.dev, sw.lba[slot], SECTORS_PER_PAGE, page) == 0
           ? 0 : -EIO;
}

int swap_read(u32 slot, void *page)
{
    if (!sw.on || slot >= sw.nslots || sw.lba[slot] == SLOT_BAD) {
        return -EINVAL;
    }
    sw.st.pageins++;
    return sw.dev->read(sw.dev, sw.lba[slot], SECTORS_PER_PAGE, page) == 0
           ? 0 : -EIO;
}

void swap_stats(struct swapstats *out)
{
    *out = sw.st;
}
