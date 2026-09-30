/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * xfertest - sendfile, splice, copy_file_range, mremap, memfd_create.
 *
 * Bytes moved are compared with a pattern that differs at every
 * offset, so a chunk moved to the wrong place cannot pass; the files
 * this leaves on the disk (/xf.dst, /xf.cfr) are compared by the host
 * against the same pattern computed there (kernel/xfertest.sh). A
 * mapping that has moved is checked through a SECOND process for the
 * old address being gone -- a fault in the child, not a belief in the
 * parent.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG      4096
#define SRCLEN  20000

static int failures, checks;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    checks++;
    if (!ok) {
        failures++;
    }
}

/* Byte i of the source file; the host computes the same. */
static unsigned char pat(unsigned i)
{
    return (unsigned char)((i * 31 + (i >> 8) * 7 + 3) & 0xff);
}

static int file_is(int fd, off_t at, unsigned from, unsigned len)
{
    unsigned char buf[PG];
    unsigned done = 0;

    while (done < len) {
        unsigned n = len - done > PG ? PG : len - done, i;

        if (pread(fd, buf, n, at + done) != (ssize_t)n) {
            return 0;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] != pat(from + done + i)) {
                return 0;
            }
        }
        done += n;
    }
    return 1;
}

/* Does touching `p` fault? Asked of a child, which dies if it does. */
static int faults(volatile unsigned char *p)
{
    int st;
    pid_t pid = fork();

    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        (void)*p;
        _exit(0);
    }
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
}

static int src_fd(void)
{
    unsigned char buf[SRCLEN];
    unsigned i;
    int fd = open("/xf.src", O_CREAT | O_RDWR | O_TRUNC, 0644);

    for (i = 0; i < SRCLEN; i++) {
        buf[i] = pat(i);
    }
    write(fd, buf, SRCLEN);
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static void sendfiles(void)
{
    int in = src_fd(), out, p[2], st;
    off_t off;
    pid_t pid;

    out = open("/xf.dst", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check("sendfile file to file, all 20000 bytes",
          sendfile(out, in, NULL, SRCLEN) == SRCLEN &&
          file_is(out, 0, 0, SRCLEN));
    check("  and the input's position moved to the end",
          lseek(in, 0, SEEK_CUR) == SRCLEN);
    check("  at the end of the input, 0", sendfile(out, in, NULL, 100) == 0);

    off = 100;
    lseek(in, 7, SEEK_SET);
    check("with an offset: those bytes, the offset advanced, the position "
          "left alone", sendfile(out, in, &off, 50) == 50 && off == 150 &&
          lseek(in, 0, SEEK_CUR) == 7 && file_is(out, SRCLEN, 100, 50));

    /* Into a pipe five times its size: it has to wait for the reader. */
    pipe(p);
    pid = fork();
    if (pid == 0) {
        unsigned char buf[PG];
        unsigned got = 0, bad = 0, i;
        ssize_t n;

        close(p[1]);
        while ((n = read(p[0], buf, sizeof(buf))) > 0) {
            for (i = 0; i < (unsigned)n; i++) {
                bad |= buf[i] != pat(got + i);
            }
            got += (unsigned)n;
        }
        _exit(got == SRCLEN && !bad ? 0 : 1);
    }
    close(p[0]);
    off = 0;
    check("into a pipe, waiting for the reader as the pipe fills",
          sendfile(p[1], in, &off, SRCLEN) == SRCLEN);
    close(p[1]);
    waitpid(pid, &st, 0);
    check("  and the reader got every byte, in order",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    {
        int ap = open("/xf.app", O_CREAT | O_WRONLY | O_APPEND, 0644);

        check("an O_APPEND output is EINVAL",
              sendfile(ap, in, NULL, 1) == -1 && errno == EINVAL);
        close(ap);
        unlink("/xf.app");
    }
    close(out);
    close(in);
}

static void splices(void)
{
    int in = src_fd(), out, p[2];
    unsigned char buf[2000];
    off_t off = 5000, off2 = 10;

    pipe(p);
    check("splice file to pipe, from an offset",
          splice(in, &off, p[1], NULL, 1000, 0) == 1000 && off == 6000 &&
          lseek(in, 0, SEEK_CUR) == 0);
    {
        unsigned i, ok = read(p[0], buf, sizeof(buf)) == 1000;

        for (i = 0; ok && i < 1000; i++) {
            ok = buf[i] == pat(5000 + i);
        }
        check("  and the pipe holds exactly those bytes", ok);
    }
    out = open("/xf.dst", O_RDWR);
    write(p[1], "splice!", 7);
    check("splice pipe to file, at an offset",
          splice(p[0], NULL, out, &off2, 7, 0) == 7 && off2 == 17 &&
          pread(out, buf, 7, 10) == 7 && memcmp(buf, "splice!", 7) == 0);
    pwrite(out, "\x00\x00\x00\x00\x00\x00\x00", 7, 10);
    {
        unsigned char fix[7];
        unsigned i;

        for (i = 0; i < 7; i++) {
            fix[i] = pat(10 + i);       /* the host checks the whole file */
        }
        pwrite(out, fix, 7, 10);
    }
    check("neither end a pipe is EINVAL",
          splice(in, NULL, out, NULL, 1, 0) == -1 && errno == EINVAL);
    off = 0;
    check("an offset for a pipe is ESPIPE",
          splice(p[0], &off, out, NULL, 1, 0) == -1 && errno == ESPIPE);
    check("SPLICE_F_NONBLOCK from an empty pipe is EAGAIN",
          splice(p[0], NULL, out, &off2, 1, SPLICE_F_NONBLOCK) == -1 &&
          errno == EAGAIN);
    close(p[0]);
    close(p[1]);
    close(out);
    close(in);
}

static void copy_ranges(void)
{
    int in = src_fd(), out;
    off_t oi = 1000, oo = 0;

    out = open("/xf.cfr", O_CREAT | O_RDWR | O_TRUNC, 0644);
    check("copy_file_range: 8000 bytes from 1000 to 0 of a new file",
          copy_file_range(in, &oi, out, &oo, 8000, 0) == 8000 &&
          oi == 9000 && oo == 8000 && file_is(out, 0, 1000, 8000));
    check("  both positions left alone",
          lseek(in, 0, SEEK_CUR) == 0 && lseek(out, 0, SEEK_CUR) == 0);
    oi = 0;
    oo = 100;
    check("  overlapping ranges of one file are EINVAL",
          copy_file_range(in, &oi, in, &oo, 1000, 0) == -1 && errno == EINVAL);
    check("  flags are EINVAL",
          copy_file_range(in, &oi, out, &oo, 1, 1) == -1 && errno == EINVAL);
    close(out);
    close(in);
}

static void mremaps(void)
{
    unsigned char *a, *b, *blk, *m;
    unsigned i;
    int ok, fd;

    a = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
             -1, 0);
    for (i = 0; i < 3 * PG; i++) {
        a[i] = pat(i);
    }
    /* Something in the way right after it, so growing must move. */
    blk = mmap(a + 3 * PG, PG, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    blk[0] = 0x5a;
    check("mremap cannot grow in place past something mapped: ENOMEM",
          blk == a + 3 * PG &&
          mremap(a, 3 * PG, 10 * PG, 0) == MAP_FAILED && errno == ENOMEM);
    b = mremap(a, 3 * PG, 10 * PG, MREMAP_MAYMOVE);
    for (ok = b != MAP_FAILED && b != a, i = 0; ok && i < 3 * PG; i++) {
        ok = b[i] == pat(i);
    }
    check("with MREMAP_MAYMOVE it moves, the contents with it", ok);
    for (ok = b != MAP_FAILED, i = 3 * PG; ok && i < 10 * PG; i++) {
        ok = b[i] == 0;
    }
    check("  the new pages are zero", ok);
    if (b != MAP_FAILED) {
        b[10 * PG - 1] = 7;
    }
    check("  and writable", b != MAP_FAILED && b[10 * PG - 1] == 7);
    check("  the old address is gone: a second process faults on it",
          faults(a));
    check("  and what was in the way is untouched", blk[0] == 0x5a);

    m = mremap(b, 10 * PG, 2 * PG, 0);
    check("shrinking stays put and cuts the tail off",
          m == b && m[PG] == pat(PG) && faults(b + 2 * PG));
    m = mremap(b, 2 * PG, 4 * PG, 0);
    check("growing where there is room stays put",
          m == b && m[PG + 5] == pat(PG + 5) && m[3 * PG] == 0);
    check("an old range with a hole in it is EFAULT",
          mremap(b, 8 * PG, 9 * PG, MREMAP_MAYMOVE) == MAP_FAILED &&
          errno == EFAULT);
    munmap(m, 4 * PG);
    munmap(blk, PG);

    /* A shared file mapping, moved, is still the file. */
    fd = open("/tmp/xf.shm", O_CREAT | O_RDWR | O_TRUNC, 0644);
    ftruncate(fd, 2 * PG);
    a = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    blk = mmap(a + 2 * PG, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
               -1, 0);
    b = mremap(a, 2 * PG, 2 * PG + 1, MREMAP_MAYMOVE);
    if (b != MAP_FAILED) {
        b[PG + 3] = 0x77;
    }
    {
        unsigned char c = 0;

        check("a MAP_SHARED mapping that has moved still writes the file",
              b != MAP_FAILED && b != a && pread(fd, &c, 1, PG + 3) == 1 &&
              c == 0x77);
    }
    close(fd);
    unlink("/tmp/xf.shm");
}

static void memfds(void)
{
    char link[80], path[32];
    unsigned char *m;
    int fd, st, n;
    struct stat s;
    DIR *d;
    struct dirent *e;
    pid_t pid;

    fd = memfd_create("hello", MFD_CLOEXEC);
    check("memfd_create", fd >= 0);
    check("  MFD_CLOEXEC sets FD_CLOEXEC", fcntl(fd, F_GETFD) == FD_CLOEXEC);
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = (int)readlink(path, link, sizeof(link) - 1);
    link[n > 0 ? n : 0] = '\0';
    check("  /proc names it /memfd:hello (deleted)",
          strcmp(link, "/memfd:hello (deleted)") == 0);
    check("  it is a file: write, read back, truncate",
          write(fd, "memory", 6) == 6 && pread(fd, link, 6, 0) == 6 &&
          memcmp(link, "memory", 6) == 0 && ftruncate(fd, 2 * PG) == 0 &&
          fstat(fd, &s) == 0 && S_ISREG(s.st_mode) && s.st_size == 2 * PG);
    m = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    pid = fork();
    if (pid == 0) {
        if (m != MAP_FAILED) {
            strcpy((char *)m + PG, "from the child");
        }
        _exit(0);
    }
    waitpid(pid, &st, 0);
    check("  mapped MAP_SHARED, shared with a child",
          m != MAP_FAILED && strcmp((char *)m + PG, "from the child") == 0);
    n = 0;
    d = opendir("/dev/shm");
    while (d && (e = readdir(d))) {
        n += strncmp(e->d_name, ".memfd", 6) == 0;
    }
    if (d) {
        closedir(d);
    }
    check("  and it has no name anywhere: nothing in /dev/shm", n == 0);
    check("  a flag it does not know is EINVAL",
          memfd_create("x", 0x100) == -1 && errno == EINVAL);
    check("  sealing is not there: F_ADD_SEALS is EINVAL",
          fcntl(fd, 1033, 1) == -1 && errno == EINVAL);
    munmap(m, 2 * PG);
    close(fd);
}

int main(void)
{
    sendfiles();
    splices();
    copy_ranges();
    mremaps();
    memfds();
    unlink("/xf.src");
    printf("xfertest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
