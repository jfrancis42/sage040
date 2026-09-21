/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * malloc.c - malloc, free, realloc and calloc.
 *
 * MEANT TO BE THROWN AWAY. The C library (progress.md task 13) brings an
 * allocator of its own; this one exists so that everything between now
 * and then has something real to allocate from. It is written to be
 * correct and checkable first, and fast enough second.
 *
 * THE SHAPE OF IT
 *
 * Memory comes from sbrk() in segments of at least 64 KB. Each segment
 * is a run of blocks between two fixed markers:
 *
 *   base + 0    next segment, or 0         } the segment header
 *   base + 4    length of this segment     }
 *   base + 8    unused, so payloads land on 8-byte boundaries
 *   base + 12   first block's header
 *   ...
 *   end - 4     the epilogue: a header of size 0, marked in use
 *
 * A block is a 4-byte header -- its size, which is a multiple of 8, and
 * three flag bits -- followed by the payload. A FREE block also keeps
 * its size in its last word (the footer) and two list links at the
 * start of its payload, so the smallest block is 16 bytes. A block in
 * use has no footer; instead the block after it records "previous in
 * use" in its own header, which is what lets free() find out whether it
 * can merge backwards without a footer to read. These are dlmalloc's
 * boundary tags, and they make every merge O(1).
 *
 * Free blocks live on one of NBINS lists by size, a list per power of
 * two. malloc() looks in the list a request belongs to, then in every
 * larger one, and takes the first block that fits. A single list would
 * be simpler and would make every call walk every free block, which an
 * editor making thousands of small allocations would feel.
 *
 * Requests of MMAP_THRESHOLD and more skip all of that and get pages of
 * their own from mmap(), as glibc does. free() hands those straight back
 * with munmap(), so a program that loads a large file and then lets it
 * go really does give the memory back.
 *
 * A free block at the very top of the heap -- touching the break --
 * that grows past TRIM_THRESHOLD is given back with a negative sbrk().
 *
 * WHAT IS CHECKED. free() and realloc() refuse a pointer whose block is
 * not in use: a double free, or a pointer malloc never returned. That
 * is reported and the program exits with 134 -- what a program killed by
 * SIGABRT reports -- because carrying on would corrupt the lists. And
 * malloc_check() walks every segment and every list and verifies all of
 * the invariants above; the tests call it constantly.
 */
#include "ulib.h"
#include "malloc.h"

#define ALIGN           8
#define MIN_BLOCK       16
#define SEG_HDR         12                  /* next, length, padding */
#define SEG_MIN         (64UL * 1024)
#define MMAP_THRESHOLD  (128UL * 1024)
#define TRIM_THRESHOLD  (256UL * 1024)
#define PAGE            4096UL

#define INUSE           0x1
#define PREV_INUSE      0x2
#define MMAPPED         0x4
#define FLAGS           0x7

#define NBINS           24

typedef struct block {
    u32 head;                   /* size | flags */
    struct block *next;         /* only while free */
    struct block *prev;
} block;

struct segment {
    struct segment *next;
    u32 len;
};

static block *bins[NBINS];
static struct segment *segments;    /* newest first */

/* Totals for mallinfo(). */
static u32 arena_bytes, mmap_bytes, mmap_count;

/* --- block arithmetic ------------------------------------------------ */

static u32 bsize(const block *b)         { return b->head & ~FLAGS; }
static int inuse(const block *b)         { return (b->head & INUSE) != 0; }
static int prev_inuse(const block *b)    { return (b->head & PREV_INUSE) != 0; }
static block *next_block(block *b)       { return (block *)((u8 *)b + bsize(b)); }
static void *payload(block *b)           { return (u8 *)b + 4; }
static block *from_payload(void *p)      { return (block *)((u8 *)p - 4); }
static u32 *footer(block *b)             { return (u32 *)((u8 *)b + bsize(b) - 4); }

/* The block before b; only meaningful when b says it is free. */
static block *prev_block(block *b)
{
    u32 psize = *(u32 *)((u8 *)b - 4);

    return (block *)((u8 *)b - psize);
}

static void set_prev_inuse(block *b, int on)
{
    if (on) {
        b->head |= PREV_INUSE;
    } else {
        b->head &= ~PREV_INUSE;
    }
}

static u32 request_size(u32 n)
{
    u32 s;

    if (n > 0x7fffff00UL) {
        return 0;               /* would overflow the arithmetic below */
    }
    s = (n + 4 + ALIGN - 1) & ~(u32)(ALIGN - 1);
    return s < MIN_BLOCK ? MIN_BLOCK : s;
}

/* --- the free lists -------------------------------------------------- */

static int bin_of(u32 size)
{
    int i = 0;

    size >>= 5;                 /* 16..31 -> bin 0 */
    while (size && i < NBINS - 1) {
        size >>= 1;
        i++;
    }
    return i;
}

static void unlink_free(block *b)
{
    int i = bin_of(bsize(b));

    if (b->prev) {
        b->prev->next = b->next;
    } else {
        bins[i] = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
}

/* Mark b free with the given size, write its footer and list it. */
static void make_free(block *b, u32 size)
{
    int i = bin_of(size);

    b->head = size | (b->head & PREV_INUSE);
    *footer(b) = size;
    set_prev_inuse(next_block(b), 0);
    b->prev = 0;
    b->next = bins[i];
    if (bins[i]) {
        bins[i]->prev = b;
    }
    bins[i] = b;
}

/*
 * Take `size` bytes from the front of b, which is free or about to be
 * used, marking them in use. What is left over, if it is big enough to
 * be a block, becomes a free block of its own.
 */
static void carve(block *b, u32 size)
{
    u32 have = bsize(b);

    if (have - size >= MIN_BLOCK) {
        block *rest = (block *)((u8 *)b + size);

        b->head = size | INUSE | (b->head & PREV_INUSE);
        rest->head = PREV_INUSE;
        make_free(rest, have - size);
    } else {
        b->head = have | INUSE | (b->head & PREV_INUSE);
        set_prev_inuse(next_block(b), 1);
    }
}

/*
 * Merge a free, UNLISTED block with free neighbours, then list it.
 * Returns the block that now holds the space.
 */
static block *coalesce(block *b)
{
    u32 size = bsize(b);
    block *n = next_block(b);

    if (!inuse(n)) {
        unlink_free(n);
        size += bsize(n);
    }
    if (!prev_inuse(b)) {
        block *p = prev_block(b);

        unlink_free(p);
        size += bsize(p);
        b = p;
    }
    make_free(b, size);
    return b;
}

/* --- getting memory from the system ---------------------------------- */

static struct segment *segment_of(block *b)
{
    struct segment *s;

    for (s = segments; s; s = s->next) {
        if ((u8 *)b > (u8 *)s && (u8 *)b < (u8 *)s + s->len) {
            return s;
        }
    }
    return 0;
}

/*
 * More heap, at least `need` bytes of block. Extends the newest segment
 * in place when the break has not moved since, and starts a new one
 * when it has -- somebody else called sbrk(), which is allowed.
 */
static int grow(u32 need)
{
    u32 len = (need + SEG_HDR + 4 + PAGE - 1) & ~(PAGE - 1);
    u8 *top = sbrk(0);
    u8 *got;

    if (len < SEG_MIN) {
        len = SEG_MIN;
    }

    if (segments && (u8 *)segments + segments->len == top) {
        /*
         * Contiguous. The old epilogue becomes the new block's header,
         * the new epilogue goes at the new end, and the block merges
         * with a free block before it if there is one.
         */
        block *b;

        got = sbrk((s32)len);
        if (got == (void *)-1) {
            return -1;
        }
        b = (block *)((u8 *)segments + segments->len - 4);
        segments->len += len;
        ((block *)((u8 *)segments + segments->len - 4))->head = INUSE;
        b->head = len | (b->head & PREV_INUSE);
        arena_bytes += len;
        coalesce(b);
        return 0;
    }

    /*
     * A new segment. The break may be anywhere -- a program is free to
     * brk() to an odd address -- and every payload must be 8-aligned,
     * so the segment starts at the next multiple of 8.
     */
    {
        u32 pad = (0U - (u32)top) & (ALIGN - 1);

        got = sbrk((s32)(len + pad));
        if (got == (void *)-1) {
            return -1;
        }
        got += pad;
    }
    {
        struct segment *s = (struct segment *)got;
        block *first = (block *)(got + SEG_HDR);
        block *epi = (block *)(got + len - 4);

        s->len = len;
        s->next = segments;
        segments = s;
        first->head = PREV_INUSE;
        epi->head = INUSE;
        arena_bytes += len;
        make_free(first, len - SEG_HDR - 4);
    }
    return 0;
}

/*
 * Give back a large free block at the top of the heap. Only the newest
 * segment can end at the break, and only if nobody has moved it since.
 */
static void trim(block *b)
{
    struct segment *s = segments;
    u8 *seg_end;
    u32 keep, give;

    if (!s || bsize(b) < TRIM_THRESHOLD) {
        return;
    }
    seg_end = (u8 *)s + s->len;
    if ((u8 *)next_block(b) != seg_end - 4 || seg_end != sbrk(0)) {
        return;
    }
    /* Keep a page of it, and keep the page arithmetic whole. */
    give = (bsize(b) - PAGE) & ~(PAGE - 1);
    if (give == 0) {
        return;
    }
    if (sbrk(-(s32)give) == (void *)-1) {
        return;
    }
    unlink_free(b);
    keep = bsize(b) - give;
    s->len -= give;
    arena_bytes -= give;
    ((block *)((u8 *)s + s->len - 4))->head = INUSE;
    b->head = keep | (b->head & PREV_INUSE);
    make_free(b, keep);
}

/* --- large blocks, straight from mmap -------------------------------- */

/*
 * A mapped block is laid out like any other, 4 bytes into its mapping
 * so the payload is 8-aligned, and its size is the whole mapping.
 */
static void *big_alloc(u32 size)
{
    u32 len = (size + 4 + PAGE - 1) & ~(PAGE - 1);
    u8 *m = mmap(0, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    block *b;

    if (m == MAP_FAILED) {
        return 0;
    }
    b = (block *)(m + 4);
    b->head = (len - 4) | INUSE | MMAPPED;
    mmap_bytes += len;
    mmap_count++;
    return payload(b);
}

static void big_free(block *b)
{
    u32 len = bsize(b) + 4;

    mmap_bytes -= len;
    mmap_count--;
    munmap((u8 *)b - 4, len);
}

/* --- the interface --------------------------------------------------- */

static void bad_pointer(const char *who, void *p)
{
    eputs(who);
    eputs("(): invalid pointer or double free at 0x");
    {
        /* puthex goes to stdout; this has to go to stderr. */
        static const char hex[] = "0123456789abcdef";
        char buf[9];
        u32 v = (u32)p;
        int i;

        for (i = 7; i >= 0; i--) {
            buf[i] = hex[v & 15];
            v >>= 4;
        }
        buf[8] = '\0';
        eputs(buf);
    }
    eputs("\n");
    exit(134);
}

static int plausible(void *p)
{
    block *b;

    if (!p || ((u32)p & (ALIGN - 1))) {
        return 0;
    }
    b = from_payload(p);
    if (!inuse(b)) {
        return 0;
    }
    return (b->head & MMAPPED) || segment_of(b) != 0;
}

void *malloc(u32 n)
{
    u32 size = request_size(n);
    int i;

    if (!size) {
        return 0;
    }
    if (size >= MMAP_THRESHOLD) {
        return big_alloc(size);
    }
    for (;;) {
        for (i = bin_of(size); i < NBINS; i++) {
            block *b;

            for (b = bins[i]; b; b = b->next) {
                if (bsize(b) >= size) {
                    unlink_free(b);
                    carve(b, size);
                    return payload(b);
                }
            }
        }
        if (grow(size) < 0) {
            return 0;
        }
    }
}

void free(void *p)
{
    block *b;

    if (!p) {
        return;
    }
    if (!plausible(p)) {
        bad_pointer("free", p);
    }
    b = from_payload(p);
    if (b->head & MMAPPED) {
        big_free(b);
        return;
    }
    b->head &= ~INUSE;
    trim(coalesce(b));
}

void *calloc(u32 count, u32 size)
{
    void *p;

    if (size && count > 0xffffffffUL / size) {
        return 0;               /* the product would wrap */
    }
    p = malloc(count * size);
    if (p) {
        memset(p, 0, count * size);
    }
    return p;
}

void *realloc(void *p, u32 n)
{
    u32 size = request_size(n);
    block *b, *nx;
    void *q;

    if (!p) {
        return malloc(n);
    }
    if (n == 0) {
        free(p);
        return 0;
    }
    if (!plausible(p)) {
        bad_pointer("realloc", p);
    }
    if (!size) {
        return 0;
    }
    b = from_payload(p);

    if (b->head & MMAPPED) {
        if (size <= bsize(b)) {
            return p;           /* it already has the pages */
        }
    } else {
        if (size <= bsize(b)) {
            /* Shrinking in place. What comes off is free, and merges. */
            u32 have = bsize(b);

            if (have - size >= MIN_BLOCK) {
                block *rest = (block *)((u8 *)b + size);

                b->head = size | INUSE | (b->head & PREV_INUSE);
                rest->head = (have - size) | PREV_INUSE;
                coalesce(rest);
            }
            return p;
        }

        /* Growing in place into a free neighbour. */
        nx = next_block(b);
        if (!inuse(nx) && bsize(b) + bsize(nx) >= size) {
            unlink_free(nx);
            b->head += bsize(nx);           /* flags are in the low bits */
            set_prev_inuse(next_block(b), 1);
            if (bsize(b) - size >= MIN_BLOCK) {
                block *rest = (block *)((u8 *)b + size);
                u32 extra = bsize(b) - size;

                b->head = size | INUSE | (b->head & PREV_INUSE);
                rest->head = extra | PREV_INUSE;
                coalesce(rest);
            }
            return p;
        }

        /*
         * Or by moving the break, when this is the last block before the
         * epilogue of the newest segment and nobody has moved the break
         * since. This is what keeps a growing buffer from being copied
         * on every step, which on a 25 MHz machine is the difference.
         */
        if (bsize(nx) == 0 && segments && segment_of(b) == segments &&
            size < MMAP_THRESHOLD &&
            (u8 *)segments + segments->len == (u8 *)sbrk(0)) {
            if (grow(size - bsize(b)) == 0) {
                nx = next_block(b);
                if (!inuse(nx) && bsize(b) + bsize(nx) >= size) {
                    return realloc(p, n);   /* the case above now fits */
                }
            }
        }
    }

    q = malloc(n);
    if (!q) {
        return 0;               /* and p is untouched, as C requires */
    }
    memcpy(q, p, bsize(b) - 4 < n ? bsize(b) - 4 : n);
    free(p);
    return q;
}

/* --- checking and reporting ------------------------------------------ */

static int complain(const char *what)
{
    eputs("malloc_check: ");
    eputs(what);
    eputs("\n");
    return -1;
}

int malloc_check(void)
{
    struct segment *s;
    u32 free_walked = 0, free_listed = 0;
    int i;

    for (s = segments; s; s = s->next) {
        block *b = (block *)((u8 *)s + SEG_HDR);
        block *end = (block *)((u8 *)s + s->len - 4);
        int prev_free = 0;

        if (!prev_inuse(b)) {
            return complain("first block does not say its predecessor is in use");
        }
        while (b != end) {
            u32 sz = bsize(b);

            if (sz < MIN_BLOCK || (sz & (ALIGN - 1)) || b > end ||
                (u8 *)b + sz > (u8 *)end) {
                return complain("a block size is impossible");
            }
            if (!!prev_inuse(b) == prev_free) {
                return complain("a PREV_INUSE bit disagrees with its neighbour");
            }
            if (!inuse(b)) {
                if (prev_free) {
                    return complain("two free blocks are adjacent");
                }
                if (*footer(b) != sz) {
                    return complain("a free block's footer is wrong");
                }
                free_walked++;
            }
            prev_free = !inuse(b);
            b = next_block(b);
        }
        if (!inuse(end) || bsize(end) != 0) {
            return complain("a segment's epilogue is damaged");
        }
        if (!!prev_inuse(end) == prev_free) {
            return complain("the epilogue's PREV_INUSE bit is wrong");
        }
    }

    for (i = 0; i < NBINS; i++) {
        block *b, *prev = 0;

        for (b = bins[i]; b; b = b->next) {
            if (inuse(b)) {
                return complain("a listed block is in use");
            }
            if (bin_of(bsize(b)) != i) {
                return complain("a block is on the wrong list");
            }
            if (b->prev != prev) {
                return complain("a list's back link is wrong");
            }
            if (!segment_of(b)) {
                return complain("a listed block is outside every segment");
            }
            prev = b;
            if (++free_listed > free_walked) {
                return complain("more blocks listed than free (a loop?)");
            }
        }
    }
    if (free_listed != free_walked) {
        return complain("a free block is missing from the lists");
    }
    return 0;
}

struct mallinfo mallinfo(void)
{
    struct mallinfo m;
    struct segment *s;
    int i;

    memset(&m, 0, sizeof(m));
    m.arena = (int)arena_bytes;
    m.hblks = (int)mmap_count;
    m.hblkhd = (int)mmap_bytes;
    for (i = 0; i < NBINS; i++) {
        block *b;

        for (b = bins[i]; b; b = b->next) {
            m.ordblks++;
            m.fordblks += (int)bsize(b);
        }
    }
    /* Everything in a segment that is not free, not a header and not an
     * epilogue is in use. */
    m.uordblks = m.arena - m.fordblks;
    for (s = segments; s; s = s->next) {
        m.uordblks -= SEG_HDR + 4;
    }
    return m;
}
