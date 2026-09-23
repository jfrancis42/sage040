/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * memtest - exercise the calls that give a program memory.
 *
 * brk, sbrk, mmap, munmap and mprotect. Each check compares against
 * something the program can see for itself -- the address of its own data, the free page
 * count, the contents of the memory -- so a wrong answer is a wrong
 * line, not a program that quietly does the wrong thing.
 *
 * The other modes each do one thing that must be a segmentation fault,
 * and only the shell can report one, so the harness checks them:
 *
 *   past      touch the page a heap shrink gave back
 *   unmapped  touch a page after munmap
 *   readonly  write to a page after mprotect(PROT_READ)
 *   none      read a page after mprotect(PROT_NONE)
 *
 * Plain `memtest` leaves a PROT_NONE page, a read-only file mapping and
 * an anonymous mapping in place when it exits, so the harness can check
 * with `free` that exit gives every one of them back.
 */
#include "ulib.h"

#define PAGE    4096UL
#define PAGE_ALIGN(p) (((u32)(p) + PAGE - 1) & ~(PAGE - 1))

static int image_data = 1;      /* somewhere in the image, to compare */

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static u32 free_pages(void)
{
    struct sysinfo si;

    if (sysinfo(&si) < 0) {
        return 0;
    }
    return si.freeram;
}

static int all_zero(const u8 *p, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (p[i]) {
            return 0;
        }
    }
    return 1;
}

static void touch_past(void)
{
    u8 *base = sbrk(0);
    volatile u8 *p;

    sbrk(4 * PAGE);
    base[0] = 1;
    sbrk(-(s32)(4 * PAGE));

    puts("memtest: touching memory the heap gave back\n");
    p = base + PAGE;
    *p = 1;
    puts("NOT-PROTECTED\n");
}

#define ANON    (MAP_PRIVATE | MAP_ANONYMOUS)

/* One mapping, set up and then abused in the way `mode` names. */
static void abuse(const char *mode)
{
    volatile u8 *p = mmap(0, PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    u8 v;

    if (p == MAP_FAILED) {
        puts("memtest: mmap failed\n");
        return;
    }
    p[0] = 7;
    if (strcmp(mode, "unmapped") == 0) {
        munmap((void *)p, PAGE);
    } else if (strcmp(mode, "readonly") == 0) {
        mprotect((void *)p, PAGE, PROT_READ);
        v = p[0];
        puts(v == 7 ? "memtest: read-only page still reads\n"
                    : "memtest: read-only page reads WRONG\n");
    } else {
        mprotect((void *)p, PAGE, PROT_NONE);
    }
    puts("memtest: abusing a ");
    puts(mode);
    puts(" page\n");
    if (strcmp(mode, "readonly") == 0) {
        p[0] = 8;
    } else {
        v = p[0];
        (void)v;
    }
    puts("NOT-PROTECTED\n");
}

static void test_mmap(void)
{
    u8 *a, *b, *c, *heap;
    u32 before, i;
    s32 r;
    int fd, ok;
    struct stat st;
    static u8 buf[PAGE];

    /* --- anonymous ------------------------------------------------- */

    before = free_pages();
    a = mmap(0, 16 * PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    report("mmap of 64 KB anonymous memory works", a != MAP_FAILED);
    report("  on a page boundary", ((u32)a & (PAGE - 1)) == 0);
    report("  above the heap and below the stack",
           (u32)a > (u32)sbrk(0) && (u32)a + 16 * PAGE <= 0x1ff00000UL);
    /* The pages come when they are touched (demand paging), so touch
     * them: a program that never does takes nothing. */
    for (i = 0; a != MAP_FAILED && i < 16; i++) {
        a[i * PAGE] = 0;
    }
    report("  and took 16 pages once touched", before - free_pages() >= 16);
    report("  reads as zero", all_zero(a, 16 * PAGE));
    for (i = 0; i < 16 * PAGE; i++) {
        a[i] = (u8)(i ^ 0x55);
    }
    ok = 1;
    for (i = 0; i < 16 * PAGE; i++) {
        ok &= a[i] == (u8)(i ^ 0x55);
    }
    report("  and holds what is written", ok);

    b = mmap(0, 3 * PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    report("a second mapping does not overlap the first",
           b != MAP_FAILED && (b + 3 * PAGE <= a || b >= a + 16 * PAGE));

    before = free_pages();
    report("munmap of the middle of a mapping works",
           munmap(a + 4 * PAGE, 4 * PAGE) == 0);
    report("  and gave its pages back", free_pages() - before >= 4);
    report("  and both ends survive it",
           a[4 * PAGE - 1] == (u8)((4 * PAGE - 1) ^ 0x55) &&
           a[8 * PAGE] == (u8)((8 * PAGE) ^ 0x55));

    /* --- protection ------------------------------------------------- */

    report("mprotect to read-only works",
           mprotect(a, PAGE, PROT_READ) == 0);
    report("  and the page still reads", a[1] == (u8)(1 ^ 0x55));
    fd = open("/memtest", O_RDONLY);
    report("  and the kernel will not read() into it",
           read(fd, a, 16) == -EFAULT);
    close(fd);

    before = free_pages();
    report("mprotect to PROT_NONE works",
           mprotect(a + PAGE, PAGE, PROT_NONE) == 0);
    report("  and the kernel will not write() from it",
           write(1, a + PAGE, 1) == -EFAULT);
    report("  and the page is still the program's, not freed",
           free_pages() == before);
    report("mprotect back to read-write works",
           mprotect(a, 2 * PAGE, PROT_READ | PROT_WRITE) == 0);
    report("  and the contents came through intact",
           a[PAGE + 5] == (u8)((PAGE + 5) ^ 0x55));
    a[0] = 1;
    report("  and it is writable again", a[0] == 1);
    report("mprotect of an unmapped page is refused with ENOMEM",
           mprotect(a + 4 * PAGE, PAGE, PROT_READ) == -ENOMEM);

    /* --- placement --------------------------------------------------- */

    c = mmap(a + 4 * PAGE, PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    report("a free hint is honoured", c == a + 4 * PAGE);
    c = mmap(a + 8 * PAGE, PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    report("  and a hint that is taken is not",
           c != MAP_FAILED && c != a + 8 * PAGE);
    c = mmap(a + 8 * PAGE, PAGE, PROT_READ | PROT_WRITE,
             ANON | MAP_FIXED, -1, 0);
    report("MAP_FIXED over an existing page works", c == a + 8 * PAGE);
    report("  and replaces it with zeroes", all_zero(c, PAGE));
    r = syscall(__NR_mmap2, (u32)(a + 9 * PAGE), PAGE,
                PROT_READ | PROT_WRITE, ANON | MAP_FIXED_NOREPLACE, -1, 0);
    report("MAP_FIXED_NOREPLACE over one is refused with EEXIST",
           r == -EEXIST);
    r = syscall(__NR_mmap2, (u32)(a + 9 * PAGE) + 1, PAGE,
                PROT_READ, ANON | MAP_FIXED, -1, 0);
    report("MAP_FIXED at an unaligned address is refused", r == -EINVAL);

    /* The heap may not grow over a mapping. */
    heap = sbrk(0);
    c = mmap((u8 *)PAGE_ALIGN(heap) + 2 * PAGE, PAGE,
             PROT_READ | PROT_WRITE, ANON | MAP_FIXED, -1, 0);
    report("a page can be mapped just above the break",
           c == (u8 *)PAGE_ALIGN(heap) + 2 * PAGE);
    report("  and the heap will not grow over it",
           sbrk(4 * PAGE) == (void *)-1 && sbrk(0) == heap);
    munmap(c, PAGE);
    report("  until it is unmapped", sbrk(4 * PAGE) == heap);
    sbrk(-(s32)(4 * PAGE));

    /* --- refusals --------------------------------------------------- */

    r = syscall(__NR_mmap2, 0, 0, PROT_READ, ANON, -1, 0);
    report("mmap of zero bytes is refused with EINVAL", r == -EINVAL);
    r = syscall(__NR_mmap2, 0, PAGE, PROT_READ,
                MAP_ANONYMOUS | MAP_SHARED | MAP_PRIVATE, -1, 0);
    report("SHARED and PRIVATE together are refused", r == -EINVAL);
    r = syscall(__NR_mmap2, 0, PAGE, PROT_READ, MAP_ANONYMOUS, -1, 0);
    report("  and neither is refused", r == -EINVAL);
    before = free_pages();
    r = syscall(__NR_mmap2, 0, 200UL * 1024 * 1024, PROT_READ, ANON, -1, 0);
    report("more memory than exists is refused with ENOMEM", r == -ENOMEM);
    report("  and cost nothing", free_pages() == before);
    report("munmap at an unaligned address is refused",
           munmap(a + 1, PAGE) == -EINVAL);

    /* --- files ------------------------------------------------------ */

    fd = open("/memtest", O_RDONLY);
    fstat(fd, &st);
    lseek(fd, 100, SEEK_SET);
    c = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    report("a file can be mapped", c != MAP_FAILED);
    report("  and the descriptor's position did not move",
           lseek(fd, 0, SEEK_CUR) == 100);
    lseek(fd, 0, SEEK_SET);
    read(fd, buf, PAGE);
    ok = c != MAP_FAILED;
    for (i = 0; ok && i < PAGE; i++) {
        ok &= c[i] == buf[i];
    }
    report("  and its first page is the file's", ok);
    report("  and past the end of the file is zero",
           c != MAP_FAILED &&
           all_zero(c + st.st_size, PAGE_ALIGN(st.st_size) - st.st_size));

    a = mmap(0, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
    lseek(fd, PAGE, SEEK_SET);
    read(fd, buf, PAGE);
    ok = a != MAP_FAILED;
    for (i = 0; ok && i < PAGE; i++) {
        ok &= a[i] == buf[i];
    }
    report("a file can be mapped from an offset", ok);

    b = mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    b[0] = 'X';
    lseek(fd, 0, SEEK_SET);
    read(fd, buf, 1);
    report("a private mapping can be written without touching the file",
           b[0] == 'X' && buf[0] == 0x7f);
    munmap(b, PAGE);

    r = syscall(__NR_mmap2, 0, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    report("MAP_SHARED read-only is allowed", r > 0);
    r = syscall(__NR_mmap2, 0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, 0);
    report("MAP_SHARED with PROT_WRITE is refused with ENODEV",
           r == -ENODEV);
    r = syscall(__NR_mmap2, 0, PAGE, PROT_READ, MAP_PRIVATE, 0, 0);
    report("mapping a terminal is refused with ENODEV", r == -ENODEV);
    r = syscall(__NR_mmap2, 0, PAGE, PROT_READ, MAP_PRIVATE, 29, 0);
    report("mapping a descriptor that is not open is EBADF", r == -EBADF);
    close(fd);
    report("a mapping outlives its descriptor", c[0] == 0x7f && c[1] == 'E');

    /* --- the old interface ------------------------------------------ */
    {
        struct mmap_arg_struct m;

        m.addr = 0;
        m.len = PAGE;
        m.prot = PROT_READ | PROT_WRITE;
        m.flags = ANON;
        m.fd = (u32)-1;
        m.offset = 0;
        r = syscall(__NR_mmap, (u32)&m);
        report("old_mmap (90) through an argument block works",
               r > 0 && all_zero((u8 *)r, PAGE));
        m.offset = 100;
        report("  and refuses an unaligned offset",
               syscall(__NR_mmap, (u32)&m) == -EINVAL);
    }

    /* Left for exit to clean up: a PROT_NONE page and the file. */
    a = mmap(0, 2 * PAGE, PROT_NONE, ANON, -1, 0);
    report("mmap with PROT_NONE works", a != MAP_FAILED);
    report("  and the kernel cannot read it either",
           write(1, a, 1) == -EFAULT);
}

int main(int argc, char **argv)
{
    u8 *start, *p, *q;
    u32 before, i;
    int ok;

    if (argc > 1 && strcmp(argv[1], "past") == 0) {
        touch_past();
        return 1;
    }
    if (argc > 1) {
        abuse(argv[1]);
        return 1;
    }

    /* --- where it starts ------------------------------------------ */

    start = sbrk(0);
    report("sbrk(0) says where the heap is",
           start != (u8 *)-1 && start != 0);
    report("  above the program's own data",
           (u32)start > (u32)&image_data);
    report("  on a page boundary", ((u32)start & (PAGE - 1)) == 0);
    report("  and far below the stack", (u32)start < 0x1f000000UL);
    report("sbrk(0) asked twice gives the same answer", sbrk(0) == start);

    /* --- growing -------------------------------------------------- */

    before = free_pages();
    p = sbrk(16 * PAGE);
    report("sbrk(64 KB) returns the old break", p == start);
    report("  and the break moved by exactly that",
           sbrk(0) == start + 16 * PAGE);
    /* Reading it touches every page, and a touched page is taken:
     * demand paging, so the zero check comes first. */
    report("new heap memory reads as zero", all_zero(p, 16 * PAGE));
    report("  and, touched, took 16 pages from the machine",
           before - free_pages() >= 16);

    for (i = 0; i < 16 * PAGE; i++) {
        p[i] = (u8)(i * 7 + 3);
    }
    ok = 1;
    for (i = 0; i < 16 * PAGE; i++) {
        if (p[i] != (u8)(i * 7 + 3)) {
            ok = 0;
        }
    }
    report("  and holds what is written to it", ok);

    /* --- a break that is not on a page boundary --------------------- */

    q = sbrk(0);
    report("brk to an unaligned address works", brk(q + 100) == 0);
    report("  and sbrk(0) reports it exactly", sbrk(0) == q + 100);
    q[99] = 0x5a;                       /* the last byte it covers */
    report("  and the byte before it is usable", q[99] == 0x5a);
    brk(q);

    /* --- shrinking ------------------------------------------------- */

    before = free_pages();
    report("sbrk(-32 KB) returns the old break",
           sbrk(-(s32)(8 * PAGE)) == start + 16 * PAGE);
    report("  and the pages went back to the machine",
           free_pages() - before >= 8);
    report("  and what is left is intact",
           p[8 * PAGE - 1] == (u8)((8 * PAGE - 1) * 7 + 3));

    /* Pages given back and taken again must be NEW pages. Whatever was
     * in them belongs to the past, and in general to somebody else. */
    sbrk(8 * PAGE);
    report("regrown pages read as zero",
           all_zero(p + 8 * PAGE, 8 * PAGE));

    /* --- refusals --------------------------------------------------- */

    q = sbrk(0);
    report("brk below the heap is refused", brk(start - PAGE) < 0);
    report("brk into the stack is refused",
           brk((void *)0x1ff00000UL) < 0);
    report("sbrk past the end of the address space is refused",
           sbrk(0x7ff00000L) == (void *)-1);
    report("  and none of those moved the break", sbrk(0) == q);

    /* More than the machine has. The kernel maps until it runs out,
     * then has to give every one of those pages back. */
    before = free_pages();
    report("sbrk of more memory than exists is refused",
           sbrk(200L * 1024 * 1024) == (void *)-1);
    report("  and the break did not move", sbrk(0) == q);
    report("  and every page it took on the way was returned",
           free_pages() == before);
    report("the heap still grows afterwards",
           sbrk(PAGE) == q && sbrk(0) == q + PAGE);

    /* --- a large heap ---------------------------------------------- */

    q = sbrk(0);
    p = sbrk(8L * 1024 * 1024);
    report("an 8 MB heap can be had", p == q);
    if (p == q) {
        p[0] = 1;
        p[8L * 1024 * 1024 - 1] = 2;
        report("  and both ends of it are usable",
               p[0] == 1 && p[8L * 1024 * 1024 - 1] == 2);
    }

    test_mmap();

    puts("memtest: done\n");
    return 0;
}
