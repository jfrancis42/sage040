/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pagetest - demand paging, copy-on-write, and swap.
 *
 * Every check compares what memory the machine says it has before and
 * after -- memctl(MEMCTL_STATS), which counts pages and faults -- with
 * what the program did, so "lazy" means "the pages were not there until
 * they were touched", measured, not assumed.
 *
 *   pagetest lazy        a big anonymous mapping, a big heap: no pages
 *                        until touched, and zeroes when they are
 *   pagetest cow         fork shares, a write copies one page, and
 *                        neither side sees the other's writes
 *   pagetest fill MB     touch MB megabytes, write a pattern into every
 *                        page, then read every page back -- with less
 *                        memory than that, only swap makes it work
 *   pagetest pair MB     two processes doing `fill` at once
 *   pagetest hog         map and touch a megabyte at a time until mmap
 *                        says no: the commit check, which refuses what
 *                        memory and swap together could not supply
 *   pagetest oom MB      two processes each map MB -- each allowed, since
 *                        neither's untouched pages count against the
 *                        other -- touch half, meet, and then touch the
 *                        rest: more than there is, so somebody must be
 *                        killed, and the machine must not be
 *   pagetest park MB S   fill MB, sleep S seconds, then check it -- a
 *                        process with pages out, for swapoff to meet
 *   pagetest forkswap MB fill MB, fork, and have both check every page:
 *                        a swapped page's slot is shared by the fork
 *   pagetest pinread MB  block in read() on a pipe while another process
 *                        fills MB -- enough to push this one's pages out
 *                        -- then have the data arrive. The kernel holds
 *                        the buffer's physical address while it sleeps;
 *                        unless that page is pinned, it is evicted and
 *                        given away meanwhile, and the data lands in
 *                        somebody else's memory
 *   pagetest drained     wait, up to 30 s, for nothing to be in swap
 *   pagetest delay MS    slow every disk request by MS (a kernel test
 *                        knob): swap I/O sleeps, and with this it sleeps
 *                        long enough for the other process to run into
 *                        whatever is half done
 *   pagetest stats       print the counters, for the harness
 */
#include "ulib.h"

#define PAGE    4096UL
#define MB      (1024UL * 1024UL)

static int failures;

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
    if (!ok) {
        failures++;
    }
}

static void say(const char *what, u32 v)
{
    puts("pagetest: ");
    puts(what);
    putch(' ');
    putdec(v);
    putch('\n');
}

static u32 num(const char *s)
{
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s++ - '0');
    }
    return v;
}

static struct memstats stats(void)
{
    struct memstats m;

    memset(&m, 0, sizeof(m));
    syscall(__NR_memctl, MEMCTL_STATS, sizeof(m), (u32)&m);
    return m;
}

#define ANON    (MAP_PRIVATE | MAP_ANONYMOUS)

/* --- lazy ------------------------------------------------------------ */

static void test_lazy(void)
{
    struct memstats m0, m1, m2;
    u8 *p, *h;
    u32 i, zero = 1;

    m0 = stats();
    p = mmap(0, 32 * MB, PROT_READ | PROT_WRITE, ANON, -1, 0);
    m1 = stats();
    report("a 32 MB anonymous mapping is granted", p != MAP_FAILED);
    say("free pages before and after mapping 32 MB:", m0.pages_free);
    say("                                           ", m1.pages_free);
    say("pages it cost:", m0.pages_free - m1.pages_free);
    /* Its page tables ARE made now -- one 256-byte table per 64 pages,
     * a page of them per 448 -- but none of its 8192 pages. */
    report("  and costs only its page tables, not its 8192 pages",
           m0.pages_free - m1.pages_free <= 8192 / 448 + 4);

    p[0] = 1;
    p[16 * MB] = 2;
    p[32 * MB - 1] = 3;
    for (i = 0; i < PAGE; i++) {
        zero &= p[8 * MB + i] == 0;     /* read, never written */
    }
    m2 = stats();
    report("touching four of its 8192 pages costs four pages, and tables",
           m1.pages_free - m2.pages_free >= 4 &&
           m1.pages_free - m2.pages_free <= 8);
    report("  each one a zero-fill fault", m2.faults_zero - m1.faults_zero >= 4);
    report("  and a page read before any write is zeroes", zero);
    report("  and what was written is there",
           p[0] == 1 && p[16 * MB] == 2 && p[32 * MB - 1] == 3);
    munmap(p, 32 * MB);
    report("munmap gives them back",
           stats().pages_free >= m2.pages_free + 4);

    m0 = stats();
    h = sbrk(16 * MB);
    m1 = stats();
    report("the heap grows by 16 MB taking only its page tables",
           h != (u8 *)-1 && m0.pages_free - m1.pages_free <= 4096 / 448 + 4);
    h[5 * MB] = 7;
    report("  and one page when one is touched",
           h[5 * MB] == 7 && stats().pages_free < m1.pages_free);
    sbrk(-(s32)(16 * MB));
}

/* --- copy on write ---------------------------------------------------- */

static void test_cow(void)
{
    u32 n = 256, i, ok = 1;                 /* 1 MB */
    u8 *p = mmap(0, n * PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    struct memstats m0, m1;
    int pipe_up[2], pipe_down[2], pid, st;
    u32 child_free[3];
    char go;

    for (i = 0; i < n; i++) {
        p[i * PAGE] = (u8)i;                /* every page real, and known */
    }
    pipe(pipe_up);
    pipe(pipe_down);
    m0 = stats();
    pid = fork();
    if (pid == 0) {
        u32 before;

        child_free[0] = stats().pages_free;
        before = stats().faults_cow;
        p[3 * PAGE] = 0xaa;                 /* one write: one copy */
        child_free[1] = stats().pages_free;
        child_free[2] = stats().faults_cow - before;
        write(pipe_up[1], child_free, sizeof(child_free));
        read(pipe_down[0], &go, 1);         /* the parent has written */
        /* The parent's write to page 5 must not be visible here, and
         * this process's to page 3 must still be its own. */
        go = (p[5 * PAGE] == 5 && p[3 * PAGE] == 0xaa) ? 'y' : 'n';
        write(pipe_up[1], &go, 1);
        exit(0);
    }
    read(pipe_up[0], child_free, sizeof(child_free));
    m1 = stats();
    say("pages the fork of a 1 MB process took:", m0.pages_free - child_free[0]);
    report("fork shares the 256 pages instead of copying them",
           m0.pages_free - child_free[0] < 64);
    report("  and the child's first write copies exactly one page",
           child_free[0] - child_free[1] == 1 && child_free[2] == 1);
    report("  which the parent does not see", p[3 * PAGE] == 3);

    p[5 * PAGE] = 0x55;
    write(pipe_down[1], "x", 1);
    read(pipe_up[0], &go, 1);
    report("  nor does the child see the parent's", go == 'y');
    waitpid(pid, &st, 0);

    for (i = 0; i < n; i++) {
        if (i != 5 && p[i * PAGE] != (u8)i) {
            ok = 0;
        }
    }
    report("the parent's memory is intact afterwards", ok && p[5 * PAGE] == 0x55);
    (void)m1;
    munmap(p, n * PAGE);
}

/* --- swap ------------------------------------------------------------- */

static u32 pattern(u32 page, u32 word)
{
    return page * 0x9e3779b9u + word * 0x85ebca6bu + 0x5a5a5a5a;
}

/* Returns the number of pages that came back wrong. */
static u32 fill(u32 mb, const char *who)
{
    u32 pages = mb * MB / PAGE, i, w, bad = 0;
    u32 *p = mmap(0, mb * MB, PROT_READ | PROT_WRITE, ANON, -1, 0);

    if (p == MAP_FAILED) {
        puts("pagetest: ");
        puts(who);
        puts(": mmap failed\n");
        return pages;
    }
    for (i = 0; i < pages; i++) {
        u32 *pg = p + i * (PAGE / 4);

        for (w = 0; w < PAGE / 4; w += 64) {
            pg[w] = pattern(i, w);
        }
    }
    for (i = 0; i < pages; i++) {
        u32 *pg = p + i * (PAGE / 4);

        for (w = 0; w < PAGE / 4; w += 64) {
            if (pg[w] != pattern(i, w)) {
                bad++;
                break;
            }
        }
    }
    munmap(p, mb * MB);
    return bad;
}

/* Write MB of pattern; returns the mapping. */
static u32 *write_pattern(u32 mb)
{
    u32 pages = mb * MB / PAGE, i, w;
    u32 *p = mmap(0, mb * MB, PROT_READ | PROT_WRITE, ANON, -1, 0);

    if (p == MAP_FAILED) {
        return 0;
    }
    for (i = 0; i < pages; i++) {
        for (w = 0; w < PAGE / 4; w += 64) {
            p[i * (PAGE / 4) + w] = pattern(i, w);
        }
    }
    return p;
}

static u32 check_pattern(const u32 *p, u32 mb)
{
    u32 pages = mb * MB / PAGE, i, w, bad = 0;

    for (i = 0; i < pages; i++) {
        for (w = 0; w < PAGE / 4; w += 64) {
            if (p[i * (PAGE / 4) + w] != pattern(i, w)) {
                bad++;
                break;
            }
        }
    }
    return bad;
}

static void park(u32 mb, u32 secs)
{
    u32 *p = write_pattern(mb);

    if (!p) {
        puts("pagetest: park: mmap failed\n");
        exit(1);
    }
    puts("pagetest: parked\n");
    msleep(secs * 1000);
    say("park: pages that came back wrong:", check_pattern(p, mb));
    exit(0);
}

static void test_forkswap(u32 mb)
{
    u32 *p = write_pattern(mb);
    struct memstats m0;
    int pid, st = 0;

    if (!p) {
        report("forkswap: mmap", 0);
        return;
    }
    /* Read it all back first, so that at the fork memory is FULL of
     * resident pages -- every one of which the fork then shares. The
     * parent's rewrite can only be made room for by evicting shared
     * pages, a mapping at a time. */
    check_pattern(p, mb);
    m0 = stats();
    pid = fork();
    if (pid == 0) {
        /* Late, so that the parent has rewritten everything first: its
         * new pages go out to swap, into whatever slots are free. */
        msleep(4000);
        exit(check_pattern(p, mb) == 0 ? 0 : 1);
    }
    {
        /* A different pattern over every page. Each is brought back,
         * written, and in time sent out again -- and the slot it came
         * from is still the child's, and must not be the one reused. */
        u32 pages = mb * MB / PAGE, i, bad = 0;

        for (i = 0; i < pages; i++) {
            p[i * (PAGE / 4)] = ~pattern(i, 0);
        }
        for (i = 0; i < pages; i++) {
            if (p[i * (PAGE / 4)] != ~pattern(i, 0)) {
                bad++;
            }
        }
        report("after a fork with pages out, the parent rewrites all of them",
               bad == 0);
    }
    waitpid(pid, &st, 0);
    report("  and the child still sees what was there when it forked",
           WIFEXITED(st) && WEXITSTATUS(st) == 0);
    report("  with pages out when it forked", m0.swap_used > 0);
}

static void test_pinread(u32 mb)
{
    int p[2], writer, filler, st_w = 0, st_f = 0;
    u8 *buf = mmap(0, PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    u32 i, got = 0, ok = 1;
    s32 n;

    buf[0] = 0;                         /* a real page, then idle */
    pipe(p);
    writer = fork();
    if (writer == 0) {
        static u8 out[PAGE];

        close(p[0]);
        msleep(3000);                   /* while the filler is running */
        for (i = 0; i < PAGE; i++) {
            out[i] = (u8)(i * 13 + 7);
        }
        write(p[1], out, PAGE);
        exit(0);
    }
    filler = fork();
    if (filler == 0) {
        u32 pass, bad = 0;

        close(p[0]);
        close(p[1]);
        /* Pressure for the whole of the reader's wait, and past the
         * write: several passes, each over more than memory. */
        for (pass = 0; pass < 4; pass++) {
            bad += fill(mb, "filler");
        }
        exit(bad == 0 ? 0 : 1);
    }
    close(p[1]);
    while (got < PAGE && (n = read(p[0], buf + got, PAGE - got)) > 0) {
        got += (u32)n;
    }
    for (i = 0; i < PAGE; i++) {
        if (buf[i] != (u8)(i * 13 + 7)) {
            ok = 0;
        }
    }
    waitpid(writer, &st_w, 0);
    waitpid(filler, &st_f, 0);
    report("data read while memory was being filled arrived intact",
           got == PAGE && ok);
    report("  and the process filling memory lost nothing to it",
           WIFEXITED(st_f) && WEXITSTATUS(st_f) == 0);
}

static void test_fill(u32 mb)
{
    struct memstats m0 = stats(), m1;
    u32 bad = fill(mb, "fill");

    m1 = stats();
    say("pages out:", m1.pageouts - m0.pageouts);
    say("pages in: ", m1.pageins - m0.pageins);
    say("pages that came back wrong:", bad);
    report("every page came back as it was written", bad == 0);
    report("  and swap was used to do it", m1.pageouts > m0.pageouts &&
                                           m1.pageins > m0.pageins);
}

static void test_pair(u32 mb)
{
    int pid = fork(), st = 0;
    u32 bad;

    if (pid == 0) {
        exit(fill(mb, "child") == 0 ? 0 : 1);
    }
    bad = fill(mb, "parent");
    waitpid(pid, &st, 0);
    report("two processes filling memory at once: the parent's pages intact",
           bad == 0);
    report("  and the child's", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void hog(void)
{
    u32 n = 0;

    for (;;) {
        u8 *p = mmap(0, MB, PROT_READ | PROT_WRITE, ANON, -1, 0);
        u32 i;

        if (p == MAP_FAILED) {
            say("mmap refused after MB:", n);
            exit(3);
        }
        for (i = 0; i < MB; i += PAGE) {
            p[i] = 1;
        }
        n++;
        if (n % 8 == 0) {
            say("touched MB:", n);
        }
    }
}

static void oom(u32 mb)
{
    int ready[2], go[2], pid, st;
    u8 *p;
    u32 i;
    char c;

    pipe(ready);                /* child -> parent */
    pipe(go);                   /* parent -> child */
    pid = fork();
    /* Only the ends each side uses, so that one of them being killed
     * is end-of-file to the other rather than a wait for ever. */
    if (pid == 0) {
        close(ready[0]);
        close(go[1]);
    } else {
        close(ready[1]);
        close(go[0]);
    }
    p = mmap(0, mb * MB, PROT_READ | PROT_WRITE, ANON, -1, 0);
    if (p == MAP_FAILED) {
        say("mmap refused, pid", (u32)getpid());
        exit(4);
    }
    /*
     * Both touch half, meet, then touch the rest. Without the meeting
     * one could touch everything and exit before the other began --
     * it happened, one run in six -- and then nobody runs out of
     * memory at all. Half each fits; all of both cannot, so once both
     * are past the meeting somebody has to be killed.
     */
    for (i = 0; i < mb * MB / 2; i += PAGE) {
        p[i] = 1;
    }
    if (pid == 0) {
        write(ready[1], "h", 1);
        read(go[0], &c, 1);
    } else {
        read(ready[0], &c, 1);
        write(go[1], "g", 1);
    }
    for (; i < mb * MB; i += PAGE) {
        p[i] = 1;
    }
    if (pid == 0) {
        exit(0);
    }
    waitpid(pid, &st, 0);
    say("the child's status:", (u32)st);
    /* The parent got here, so if anyone died it was the child. */
    puts("pagetest: parent survived\n");
    exit(WIFSIGNALED(st) || WEXITSTATUS(st) != 0 ? 0 : 5);
}

static void print_stats(void)
{
    struct memstats m = stats();

    say("free", m.pages_free);
    say("swap-slots", m.swap_slots);
    say("swap-used", m.swap_used);
    say("pageouts", m.pageouts);
    say("pageins", m.pageins);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "lazy") == 0) {
        test_lazy();
    } else if (argc > 1 && strcmp(argv[1], "cow") == 0) {
        test_cow();
    } else if (argc > 2 && strcmp(argv[1], "fill") == 0) {
        test_fill(num(argv[2]));
    } else if (argc > 2 && strcmp(argv[1], "pair") == 0) {
        test_pair(num(argv[2]));
    } else if (argc > 3 && strcmp(argv[1], "park") == 0) {
        park(num(argv[2]), num(argv[3]));
    } else if (argc > 2 && strcmp(argv[1], "forkswap") == 0) {
        test_forkswap(num(argv[2]));
    } else if (argc > 2 && strcmp(argv[1], "pinread") == 0) {
        test_pinread(num(argv[2]));
    } else if (argc > 2 && strcmp(argv[1], "oom") == 0) {
        oom(num(argv[2]));
    } else if (argc > 1 && strcmp(argv[1], "hog") == 0) {
        hog();
    } else if (argc > 2 && strcmp(argv[1], "delay") == 0) {
        syscall(__NR_kstat, KSTAT_DISK_DELAY, num(argv[2]), 0);
        return 0;
    } else if (argc > 1 && strcmp(argv[1], "drained") == 0) {
        u32 t;

        for (t = 0; t < 300 && stats().swap_used != 0; t++) {
            msleep(100);
        }
        puts(stats().swap_used == 0 ? "pagetest: swap drained\n"
                                    : "pagetest: swap NOT drained\n");
        return 0;
    } else if (argc > 1 && strcmp(argv[1], "stats") == 0) {
        print_stats();
        return 0;
    } else {
        puts("usage: pagetest lazy|cow|fill MB|pair MB|hog|stats\n");
        return 2;
    }
    puts("pagetest: ");
    putdec((u32)failures);
    puts(" failed\n");
    return failures != 0;
}
