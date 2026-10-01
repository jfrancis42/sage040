/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * kstackprobe - make the kernel use KB kilobytes of this task's kernel
 * stack (kstat KSTAT_STACK_PROBE), and say whether it came back. Run by
 * kernel/stacktest.sh; enough kilobytes runs the stack out, and the
 * report that makes is what that suite checks.
 *
 *   kstackprobe KB
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#define NR_KSTAT          1005
#define KSTAT_STACK       3
#define KSTAT_STACK_PROBE 7

struct kstackstats {
    unsigned long size, max_used;
    char name[16];
};

int main(int argc, char **argv)
{
    struct kstackstats ks;
    long kb, r;

    if (argc != 2) {
        fprintf(stderr, "usage: kstackprobe KB\n");
        return 2;
    }
    kb = strtol(argv[1], 0, 10);
    printf("probe %ld KB\n", kb);
    fflush(stdout);
    r = syscall(NR_KSTAT, KSTAT_STACK_PROBE, kb, 0);
    if (r < 0) {
        perror("kstat");
        return 1;
    }
    if (syscall(NR_KSTAT, KSTAT_STACK, sizeof(ks), &ks) == 0) {
        printf("back: deepest %lu of %lu bytes, by %s\n", ks.max_used,
               ks.size, ks.name);
    }
    return 0;
}
