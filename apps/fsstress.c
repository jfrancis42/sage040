/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fsstress - the filesystem used by several processes at once.
 *
 * Before the disk was interrupt-driven, nothing ever slept inside the
 * filesystem, so two processes could never be in it together. Now a
 * process waiting for a sector sleeps in the middle of a FAT operation
 * and another runs -- which is safe only because of the filesystem's
 * lock (vfs.c). This makes that happen as much as it can:
 *
 *   three writers, each writing a file of its own in the same directory
 *   in small pieces, then reading it back;
 *   three processes creating, writing and deleting small files in that
 *   same directory the whole time;
 *
 * and then checks every byte. The harness checks the volume afterwards
 * with the host's fsck.fat, which sees what this cannot.
 */
#include "ulib.h"

#define WRITERS   3
#define CHURNERS  3
#define FILE_KB   96
#define CHUNK     700               /* odd, so writes straddle sectors */

static u8 pattern(u32 who, u32 i)
{
    return (u8)(who * 37 + i * 11 + (i >> 9));
}

static void name_of(char *out, const char *prefix, u32 n)
{
    char *p = out;

    while (*prefix) {
        *p++ = *prefix++;
    }
    *p++ = (char)('0' + (n / 100) % 10);
    *p++ = (char)('0' + (n / 10) % 10);
    *p++ = (char)('0' + n % 10);
    *p = '\0';
}

static int writer(u32 who)
{
    static u8 buf[CHUNK];
    char name[32];
    u32 total = FILE_KB * 1024, done = 0, i;
    int fd, bad = 0;

    name_of(name, "/STRESS/W", who);
    fd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return 1;
    }
    while (done < total) {
        u32 n = total - done < CHUNK ? total - done : CHUNK;

        for (i = 0; i < n; i++) {
            buf[i] = pattern(who, done + i);
        }
        if (write(fd, buf, n) != (s32)n) {
            close(fd);
            return 2;
        }
        done += n;
    }
    close(fd);

    fd = open(name, O_RDONLY);
    if (fd < 0) {
        return 3;
    }
    done = 0;
    for (;;) {
        s32 n = read(fd, buf, CHUNK);

        if (n <= 0) {
            break;
        }
        for (i = 0; i < (u32)n; i++) {
            if (buf[i] != pattern(who, done + i)) {
                bad = 1;
            }
        }
        done += (u32)n;
    }
    close(fd);
    return (bad || done != total) ? 4 : 0;
}

static int churner(u32 who)
{
    char name[32];
    u32 k;
    int bad = 0;

    for (k = 0; k < 90; k++) {
        int fd;
        char in[16];

        /* Each churner its own names, all in the one directory, so the
         * directory's slots are what they contend for. */
        name_of(name, "/STRESS/T", who * 100 + k % 25);
        fd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            return 5;
        }
        write(fd, name, 12);
        close(fd);
        fd = open(name, O_RDONLY);
        if (fd < 0 || read(fd, in, 12) != 12 || memcmp(in, name, 12) != 0) {
            bad = 1;
        }
        if (fd >= 0) {
            close(fd);
        }
        if (k % 3 == 2) {
            unlink(name);
        }
    }
    return bad ? 6 : 0;
}

int main(int argc, char **argv)
{
    int pids[WRITERS + CHURNERS], i, st, failed = 0;
    u32 delay = 0;

    /* fsstress MS: every disk request sleeps MS first (a kernel test
     * knob), so that processes really are asleep inside the filesystem
     * while others use it. */
    if (argc > 1) {
        for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) {
            delay = delay * 10 + (u32)(argv[1][i] - '0');
        }
        syscall(__NR_kstat, KSTAT_DISK_DELAY, delay, 0);
    }
    mkdir("/STRESS");
    for (i = 0; i < WRITERS + CHURNERS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            exit(i < WRITERS ? writer((u32)i) : churner((u32)(i - WRITERS)));
        }
    }
    for (i = 0; i < WRITERS + CHURNERS; i++) {
        waitpid(pids[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            puts("fsstress: process ");
            putdec((u32)i);
            puts(" failed, status ");
            putdec((u32)st);
            putch('\n');
            failed++;
        }
    }
    syscall(__NR_kstat, KSTAT_DISK_DELAY, 0, 0);
    puts(failed ? "fsstress: FAILED\n" : "fsstress: every byte of every file intact\n");
    return failed != 0;
}
