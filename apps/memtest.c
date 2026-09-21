/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * memtest - exercise the calls that give a program memory.
 *
 * brk and sbrk today. Each check compares against something the program
 * can see for itself -- the address of its own data, the free page
 * count, the contents of the memory -- so a wrong answer is a wrong
 * line, not a program that quietly does the wrong thing.
 *
 * `memtest past` does one thing: shrinks the heap and then touches the
 * page it just gave back. That must be a segmentation fault, and only
 * the shell can report one, so the harness checks it rather than this
 * program.
 */
#include "ulib.h"

#define PAGE    4096UL

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

int main(int argc, char **argv)
{
    u8 *start, *p, *q;
    u32 before, i;
    int ok;

    if (argc > 1 && strcmp(argv[1], "past") == 0) {
        touch_past();
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
    report("  and took 16 pages from the machine",
           before - free_pages() >= 16);
    report("new heap memory reads as zero", all_zero(p, 16 * PAGE));

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

    puts("memtest: done\n");
    return 0;
}
