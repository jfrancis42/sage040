/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * shmaptest - MAP_SHARED of a file, written through.
 *
 * Every check compares what a shared mapping holds with something that
 * is not that mapping: read() and write() on the same file, a forked
 * child writing through its copy of the mapping, a second process that
 * mapped the file for itself, a second mapping in the same process. A
 * mapping that was really a private copy -- which is what MAP_SHARED
 * of a file used to be here -- passes none of them.
 *
 * The last thing it does is write a pattern into /persist.dat through a
 * mapping ONLY, munmap it and exit; kernel/shmaptest.sh then reads that
 * file off the disk image on the host, with the host's tools, which is
 * the one witness that cannot agree with the kernel by mistake.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG      4096
#define FILE1   "/tmp/shm.dat"

static int failures, checks;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    checks++;
    if (!ok) {
        failures++;
    }
}

/* What byte i of the persistence file must be: not a run of one value,
 * so a page written at the wrong offset cannot pass. */
static unsigned char pattern(unsigned i)
{
    return (unsigned char)((i * 7 + (i >> 12) * 13 + 1) & 0xff);
}

static int pread_byte(int fd, off_t off)
{
    unsigned char c;

    return pread(fd, &c, 1, off) == 1 ? c : -1;
}

static void basics(void)
{
    int fd, fd2, st;
    unsigned char *m, *m2, buf[64];
    pid_t pid;
    int i, ok;

    unlink(FILE1);
    fd = open(FILE1, O_CREAT | O_RDWR, 0644);
    for (i = 0; i < 3 * PG; i++) {
        buf[0] = (unsigned char)(i & 0xff);
        write(fd, buf, 1);
    }
    m = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap MAP_SHARED, PROT_WRITE of a file works", m != MAP_FAILED);
    if (m == MAP_FAILED) {
        return;
    }
    for (ok = 1, i = 0; i < 3 * PG; i++) {
        ok = ok && m[i] == (unsigned char)(i & 0xff);
    }
    check("  it holds what write() put in the file", ok);

    m[10] = 0xa1;
    m[PG + 20] = 0xa2;
    check("msync(MS_SYNC) works", msync(m, 3 * PG, MS_SYNC) == 0);
    fd2 = open(FILE1, O_RDONLY);
    check("  and read() on another descriptor sees what the mapping wrote",
          pread_byte(fd2, 10) == 0xa1 && pread_byte(fd2, PG + 20) == 0xa2);

    m[30] = 0xb3;
    check("read() sees a mapping's write without msync, as on Linux",
          pread_byte(fd2, 30) == 0xb3);

    buf[0] = 0xc4;
    pwrite(fd, buf, 1, 2 * PG + 5);
    check("the mapping sees what write() puts in the file",
          m[2 * PG + 5] == 0xc4);

    /* A child's writes through the mapping it inherited. */
    pid = fork();
    if (pid == 0) {
        m[40] = 0xd5;
        _exit(0);
    }
    waitpid(pid, &st, 0);
    check("a forked child's store through the mapping is the parent's too",
          m[40] == 0xd5);

    /* Another process, which opened and mapped the file itself. */
    pid = fork();
    if (pid == 0) {
        int cfd = open(FILE1, O_RDWR);
        unsigned char *cm = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED,
                                 cfd, 0);

        if (cm == MAP_FAILED) {
            _exit(2);
        }
        cm[50] = 0xe6;
        _exit(cm[10] == 0xa1 ? 0 : 3);  /* and it sees ours */
    }
    waitpid(pid, &st, 0);
    check("a process that mapped the file itself shares the page both ways",
          WIFEXITED(st) && WEXITSTATUS(st) == 0 && m[50] == 0xe6);

    /* Two mappings in one process: one page. */
    m2 = mmap(0, PG, PROT_READ, MAP_SHARED, fd2, 0);
    m[60] = 0xf7;
    check("a second mapping, read-only, of a read-only descriptor sees it",
          m2 != MAP_FAILED && m2[60] == 0xf7);
    check("PROT_WRITE with MAP_SHARED on a read-only descriptor is EACCES",
          mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0) == MAP_FAILED &&
          errno == EACCES);

    /* mprotect does not turn it into a copy. */
    mprotect(m, PG, PROT_READ);
    mprotect(m, PG, PROT_READ | PROT_WRITE);
    m[70] = 0x18;
    check("after mprotect read-only and back, a store still reaches the file",
          pread_byte(fd2, 70) == 0x18);

    {
        char line[256];
        FILE *mf = fopen("/proc/self/maps", "r");
        int found = 0;

        while (mf && fgets(line, sizeof(line), mf)) {
            unsigned long lo = strtoul(line, 0, 16);

            if (lo == (unsigned long)m && strstr(line, "rw-s")) {
                found = 1;
            }
        }
        if (mf) {
            fclose(mf);
        }
        check("/proc/self/maps shows it shared, rw-s", found);
    }

    /* Past the end: the page is there, the file does not grow. */
    ftruncate(fd, PG + 100);
    check("after a truncate, what is past the new end reads zero",
          m[PG + 100] == 0 && m[PG + 99] == (unsigned char)((PG + 99) & 0xff));
    m[PG + 200] = 0x29;
    msync(m, 3 * PG, MS_SYNC);
    {
        struct stat s;

        fstat(fd, &s);
        check("  and a store there does not make the file longer",
              s.st_size == PG + 100);
    }

    check("msync of a range that is not mapped is ENOMEM",
          msync((void *)0x1f000000, PG, MS_SYNC) == -1 && errno == ENOMEM);

    munmap(m2, PG);
    m[80] = 0x3a;
    munmap(m, 3 * PG);
    close(fd);
    check("munmap writes back what msync was never asked for",
          pread_byte(fd2, 80) == 0x3a);
    close(fd2);
}

/* A mapping that outlives its descriptor and its name. */
static void unlinked(void)
{
    int fd = open("/tmp/gone.dat", O_CREAT | O_RDWR | O_TRUNC, 0644);
    unsigned char *m;

    ftruncate(fd, PG);
    m = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    unlink("/tmp/gone.dat");
    if (m != MAP_FAILED) {
        m[0] = 1;
        m[PG - 1] = 2;
    }
    check("a mapping outlives its descriptor and the file's name",
          m != MAP_FAILED && m[0] == 1 && m[PG - 1] == 2 &&
          munmap(m, PG) == 0);
}

/* The file the host reads: written only through a mapping, by a child
 * that then simply exits -- no msync, no munmap. */
static void persist(void)
{
    int st;
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open("/persist.dat", O_CREAT | O_RDWR | O_TRUNC, 0644);
        unsigned char *m;
        unsigned i;

        ftruncate(fd, 5 * PG + 123);
        m = mmap(0, 6 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) {
            _exit(1);
        }
        for (i = 0; i < 5 * PG + 123; i++) {
            m[i] = pattern(i);
        }
        _exit(0);               /* the exit writes it back */
    }
    waitpid(pid, &st, 0);
    {
        int fd = open("/persist.dat", O_RDONLY);
        unsigned char c;
        unsigned i;
        int ok = fd >= 0;

        for (i = 0; ok && i < 5 * PG + 123; i += 997) {
            ok = pread(fd, &c, 1, i) == 1 && c == pattern(i);
        }
        close(fd);
        check("a process that wrote a file only through a mapping and exited: "
              "the file holds it", WIFEXITED(st) && WEXITSTATUS(st) == 0 && ok);
    }
}

int main(void)
{
    basics();
    unlinked();
    persist();
    sync();
    printf("shmaptest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
