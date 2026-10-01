/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * jtest - the journal's side of kernel/journaltest.sh.
 *
 *   jtest stats              on, commits, forced, replayed
 *   jtest stop HOW N         the N'th commit from now stops the machine:
 *                            HOW 1 after its commit block, 2 before it
 *   jtest sync               sync(2): commit what there is
 *   jtest write PATH N       PATH holds N copies of a line naming it
 *   jtest check PATH N       ... and does it? exit 0 if so
 *   jtest churn DIR OPS SEED a deterministic storm of creates, appends,
 *                            renames, links, symlinks, truncates, unlinks,
 *                            mkdirs and rmdirs, with a sync every 25
 *
 * kstat (1005) is this kernel's own call; see KSTAT_JOURNAL in uapi.h.
 */
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NR_KSTAT            1005
#define KSTAT_JOURNAL       5
#define KSTAT_JOURNAL_STOP  6

struct journalstats {
    unsigned long on, commits, forced, replayed;
};

static void line_of(char *out, size_t size, const char *path, int i)
{
    snprintf(out, size, "%s line %d of the journal test\n", path, i);
}

static int do_write(const char *path, int n)
{
    char line[256];
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), i;

    if (fd < 0) {
        perror(path);
        return 1;
    }
    for (i = 0; i < n; i++) {
        line_of(line, sizeof(line), path, i);
        if (write(fd, line, strlen(line)) != (ssize_t)strlen(line)) {
            perror("write");
            return 1;
        }
    }
    return close(fd) != 0;
}

static int do_check(const char *path, int n)
{
    char line[256], got[256];
    int fd = open(path, O_RDONLY), i;

    if (fd < 0) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        size_t len;

        line_of(line, sizeof(line), path, i);
        len = strlen(line);
        if (read(fd, got, len) != (ssize_t)len || memcmp(got, line, len) != 0) {
            close(fd);
            return 1;
        }
    }
    i = (int)read(fd, got, 1);
    close(fd);
    return i != 0;
}

/* 32 bits on purpose: the same sequence here and on a 64-bit host,
 * which runs the same churn for kernel/linuxfstest.sh to compare. */
static uint32_t rng;

static unsigned long rnd(unsigned long n)
{
    rng = rng * 1103515245U + 12345U;
    return (unsigned long)((rng >> 8) % n);
}

/* churn's log: one line per call, its arguments relative to DIR, what it
 * returned and errno -- two runs that differ diff at the first call two
 * kernels disagree about. */
static FILE *clog;
static size_t cdir;

/* By NAME: the numbers are the C library's, and picolibc's are not
 * glibc's (ENOTEMPTY is 90 here and 39 there). */
static const char *errname(int e)
{
    switch (e) {
    case 0:         return "0";
    case ENOENT:    return "ENOENT";
    case EEXIST:    return "EEXIST";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EISDIR:    return "EISDIR";
    case ENOTDIR:   return "ENOTDIR";
    case EINVAL:    return "EINVAL";
    case ELOOP:     return "ELOOP";
    case EPERM:     return "EPERM";
    case EACCES:    return "EACCES";
    case EXDEV:     return "EXDEV";
    case ENOSPC:    return "ENOSPC";
    case EBUSY:     return "EBUSY";
    case EMLINK:    return "EMLINK";
    default:        return "E?";
    }
}

static int note(int k, const char *op, const char *a, const char *b, int r)
{
    if (clog) {
        int e = r < 0 ? errno : 0;

        fprintf(clog, "%d %s %s %s = %d %s\n", k, op, a + cdir,
                b ? b + cdir : "-", r, errname(e));
        errno = e;
    }
    return r;
}

static int do_churn(const char *dir, int ops, unsigned long seed, const char *logpath)
{
    char a[256], b[256], buf[3000];
    int k;

    clog = logpath ? fopen(logpath, "w") : 0;
    cdir = strlen(dir) + 1;

    rng = (uint32_t)seed;
    mkdir(dir, 0755);
    for (k = 0; k < ops; k++) {
        int fd;
        unsigned long x = rnd(40), y = rnd(40), len;

        snprintf(a, sizeof(a), "%s/f%lu", dir, x);
        snprintf(b, sizeof(b), "%s/f%lu", dir, y);
        switch (rnd(10)) {
        case 0: case 1:                     /* create or rewrite */
            len = rnd(sizeof(buf));
            memset(buf, (int)('a' + k % 26), len);
            fd = note(k, "create", a, 0, open(a, O_WRONLY | O_CREAT | O_TRUNC, 0640));
            if (fd >= 0) {
                note(k, "write", a, 0, (int)write(fd, buf, len));
                close(fd);
            }
            break;
        case 2:                             /* append */
            len = rnd(sizeof(buf));
            memset(buf, (int)('A' + k % 26), len);
            fd = note(k, "append", a, 0, open(a, O_WRONLY | O_CREAT | O_APPEND, 0640));
            if (fd >= 0) {
                note(k, "write", a, 0, (int)write(fd, buf, len));
                close(fd);
            }
            break;
        case 3:
            note(k, "rename", a, b, rename(a, b));
            break;
        case 4:
            note(k, "link", a, b, link(a, b));
            break;
        case 5:
            snprintf(buf, sizeof(buf), "target-%d-of-a-link-%s", k,
                     rnd(2) ? "short" :
                     "long-enough-that-it-does-not-fit-in-the-inode-itself");
            note(k, "symlink", b, 0, symlink(buf, b));
            break;
        case 6:
            note(k, "truncate", a, 0, truncate(a, (off_t)rnd(5000)));
            break;
        case 7:
            note(k, "unlink", a, 0, unlink(a));
            break;
        case 8:
            snprintf(a, sizeof(a), "%s/d%lu", dir, x % 8);
            if (note(k, "mkdir", a, 0, mkdir(a, 0755)) != 0) {
                note(k, "rmdir", a, 0, rmdir(a));
            }
            break;
        default:                            /* into and out of a subdirectory */
            snprintf(b, sizeof(b), "%s/d%lu/g%lu", dir, x % 8, y);
            if (note(k, "rename", a, b, rename(a, b)) != 0) {
                note(k, "rename", b, a, rename(b, a));
            }
            break;
        }
        if (k % 25 == 24) {
            sync();
            printf("CHURN %d\n", k + 1);
            fflush(stdout);
        }
    }
    sync();
    if (clog) {
        fclose(clog);
    }
    printf("CHURN-DONE %d\n", ops);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "stats")) {
        struct journalstats js;

        memset(&js, 0, sizeof(js));
        if (syscall(NR_KSTAT, KSTAT_JOURNAL, sizeof(js), &js) != 0) {
            perror("kstat");
            return 1;
        }
        printf("journal on=%lu commits=%lu forced=%lu replayed=%lu\n",
               js.on, js.commits, js.forced, js.replayed);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "stop")) {
        return syscall(NR_KSTAT, KSTAT_JOURNAL_STOP, atol(argv[2]),
                       atol(argv[3])) != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "sync")) {
        sync();
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "write")) {
        return do_write(argv[2], atoi(argv[3]));
    }
    if (argc == 4 && !strcmp(argv[1], "check")) {
        return do_check(argv[2], atoi(argv[3]));
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "churn")) {
        return do_churn(argv[2], atoi(argv[3]), strtoul(argv[4], 0, 10),
                        argc == 6 ? argv[5] : 0);
    }
    fprintf(stderr, "usage: jtest stats|stop HOW N|sync|write P N|check P N|churn DIR OPS SEED\n");
    return 2;
}
