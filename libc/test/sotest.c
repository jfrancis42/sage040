/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sotest - shared libraries: that a program linked against two of them
 * runs, and that their text is shared memory rather than shared files.
 *
 *   sotest             the whole test, which runs itself again as below
 *   sotest page ADDR   print the physical page behind ADDR (hex), as
 *                      a separately exec'd process sees it
 *   sotest value       print sot_value() and nothing else
 *   sotest overwrite SRC DST
 *                      write SRC's bytes over DST in place -- open
 *                      without O_TRUNC, so it is the write itself, not
 *                      a truncate, that the kernel has to notice
 *   sotest mapfirst FILE
 *                      map FILE's first page read-only -- the way ld.so
 *                      maps a library's text -- and print its first word
 *   sotest truncate FILE
 *                      open FILE with O_TRUNC and close it, writing nothing
 *
 * The physical pages come from memctl(MEMCTL_PAGE), about the caller's
 * own addresses. libc.so and libsot.so are loaded at the same addresses
 * in every process -- each process maps them in the same order into
 * the same empty space -- which is what lets one process ask another
 * about "the page at this address" and compare the answers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>

extern long syscall(long nr, ...);      /* picolibc's, in libos/linux */

#define NR_memctl    1004
#define MEMCTL_STATS 1
#define MEMCTL_PAGE  2

struct memstats {
    unsigned pages_total, pages_free;
    unsigned tc_cached, tc_hits, tc_misses, tc_evicted, tc_forgotten;
};

struct pageinfo {
    unsigned pa, refs, writable;
};

extern int sot_value(void);
extern int sot_bump(void);
extern int sot_ctor_ran(void);
extern int sot_print(const char *s);
extern int sot_counter;
extern const void *sot_libc_text(void);
extern const void *sot_own_text(void);
extern int sot_bsearch_works(void);
extern const void *sot_value_addr(void);

static int failures;

static void report(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static struct pageinfo page(const void *va)
{
    struct pageinfo pi;

    memset(&pi, 0, sizeof(pi));
    syscall(NR_memctl, MEMCTL_PAGE, (unsigned long)va, &pi);
    return pi;
}

static struct memstats stats(void)
{
    struct memstats m;

    memset(&m, 0, sizeof(m));
    syscall(NR_memctl, MEMCTL_STATS, 0, &m);
    return m;
}

/* Run this program again with ARG, and read the one number it prints. */
static unsigned ask_other(const char *self, const char *a1, const char *a2)
{
    int p[2], st;
    char buf[32];
    ssize_t n;
    pid_t pid;

    if (pipe(p) < 0) {
        return 0;
    }
    pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        execl(self, self, a1, a2, (char *)0);
        _exit(127);
    }
    close(p[1]);
    n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    waitpid(pid, &st, 0);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    return (unsigned)strtoul(buf, 0, 16);
}

int main(int argc, char **argv)
{
    const void *libc_text = sot_libc_text();
    const void *sot_text = sot_own_text();
    struct pageinfo mine, before, after;
    struct memstats m0, m1;
    unsigned other;

    if (argc > 2 && strcmp(argv[1], "page") == 0) {
        printf("%x\n", page((const void *)strtoul(argv[2], 0, 16)).pa);
        return 0;
    }
    if (argc > 3 && strcmp(argv[1], "overwrite") == 0) {
        static char buf[4096];
        int in = open(argv[2], O_RDONLY), out = open(argv[3], O_WRONLY);
        ssize_t n, total = 0;

        if (in < 0 || out < 0) {
            printf("sotest: cannot open\n");
            return 1;
        }
        while ((n = read(in, buf, sizeof(buf))) > 0) {
            if (write(out, buf, n) != n) {
                printf("sotest: short write\n");
                return 1;
            }
            total += n;
        }
        close(in);
        close(out);
        printf("sotest: overwrote %ld bytes\n", (long)total);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "mapfirst") == 0) {
        int fd = open(argv[2], O_RDONLY);
        const unsigned char *p;

        if (fd < 0) {
            printf("sotest: cannot open\n");
            return 1;
        }
        {
            struct stat st;

            if (fstat(fd, &st) == 0) {
                printf("sotest: inode %lx\n", (unsigned long)st.st_ino);
            }
        }
        p = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            printf("sotest: cannot map\n");
            return 1;
        }
        printf("sotest: first word %02x%02x%02x%02x\n", p[0], p[1], p[2], p[3]);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "truncate") == 0) {
        int fd = open(argv[2], O_WRONLY | O_TRUNC);

        if (fd < 0) {
            return 1;
        }
        close(fd);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "value") == 0) {
        printf("sotest: value %d\n", sot_value());
        return 0;
    }

    printf("sotest: two shared libraries, one of them libc\n");

    /* --- it runs ----------------------------------------------------- */
    report("a function in libsot.so", sot_value() == 1 || sot_value() == 2);
    report("its constructor ran before main", sot_ctor_ran());
    report("a variable of the library's, read by the program",
           sot_counter == 41);
    report("  and the library's own code sees the same variable",
           sot_bump() == 42 && sot_counter == 42);
    report("a library calling into libc.so", sot_print("hello") == 14);
    report("a function has one address, in the program and in the library",
           (const void *)&sot_value == sot_value_addr());

    /* --- the text is shared memory ----------------------------------- */
    mine = page(libc_text);
    printf("sotest: bsearch at %p, page %x, %u holders\n", libc_text,
           mine.pa, mine.refs);
    report("libc's text is read-only", mine.pa && !mine.writable);
    report("  and held by more than this process (the cache, at least)",
           mine.refs >= 2);

    {
        char addr[16];

        snprintf(addr, sizeof(addr), "%lx", (unsigned long)libc_text);
        m0 = stats();
        other = ask_other(argv[0], "page", addr);
        m1 = stats();
    }
    printf("sotest: another process's page %x\n", other);
    report("another program, exec'd separately, gets the SAME page of libc",
           other == mine.pa);
    report("  which it found in the cache rather than reading the file",
           m1.tc_hits > m0.tc_hits);

    {
        char addr[16];

        snprintf(addr, sizeof(addr), "%lx", (unsigned long)sot_text);
        report("and the same page of libsot's text",
               ask_other(argv[0], "page", addr) == page(sot_text).pa);
    }

    /* --- data is not ------------------------------------------------- */
    {
        char addr[16];
        struct pageinfo d = page(&sot_counter);

        snprintf(addr, sizeof(addr), "%lx", (unsigned long)&sot_counter);
        other = ask_other(argv[0], "page", addr);
        report("the library's data is this process's own",
               d.writable && d.refs == 1 && other && other != d.pa);
    }

    /* --- fork shares, and a write gets a copy ------------------------ */
    {
        static unsigned char snapshot[4096];
        int up[2], down[2], st;         /* child to parent, and back */
        unsigned child_pa = 0, again = 0;
        void *pg = (void *)((unsigned long)libc_text & ~4095UL);
        char go;
        pid_t pid;

        before = page(libc_text);
        memcpy(snapshot, pg, sizeof(snapshot));
        pipe(up);
        pipe(down);
        pid = fork();
        if (pid == 0) {
            unsigned pa = page(libc_text).pa;

            write(up[1], &pa, sizeof(pa));
            /* Told when the parent has its copy; then look again: the
             * parent's copy must not have moved this one. */
            read(down[0], &go, 1);
            pa = page(libc_text).pa;
            write(up[1], &pa, sizeof(pa));
            _exit(0);
        }
        read(up[0], &child_pa, sizeof(child_pa));
        report("a forked child shares the parent's page of libc's text",
               child_pa == before.pa);

        /* One page of libc's text made writable here: this process, and
         * only this one, gets its own copy of it. */
        {
            int r = mprotect(pg, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);

            after = page(libc_text);
            report("mprotect(PROT_WRITE) on shared text succeeds", r == 0);
            report("  and gives this process its own copy of the page",
                   after.writable && after.pa != before.pa &&
                   after.refs == 1);
            report("  with the same contents",
                   memcmp(snapshot, pg, sizeof(snapshot)) == 0);
        }
        write(down[1], "x", 1);
        read(up[0], &again, sizeof(again));
        report("  while the child still has the shared one",
               again == before.pa);
        waitpid(pid, &st, 0);
        report("libc still works from the copied page",
               sot_bsearch_works());
    }

    printf("sotest: %d failed\n", failures);
    printf("sotest: done\n");
    return failures != 0;
}
