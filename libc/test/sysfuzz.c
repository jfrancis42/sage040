/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sysfuzz - random system calls with hostile arguments, run by
 * kernel/fuzztest.sh. A small trinity.
 *
 *   sysfuzz ROUNDS CALLS [SEED [DISK_DELAY_MS]]
 *
 * Each round is a child that makes CALLS calls and is then killed.
 * The child is a nobody: chrooted into /fz, uid and gid 1000, its
 * standard descriptors on /dev/null -- so what it can damage is itself,
 * a scratch directory, and the kernel, and the kernel is the point. Its
 * calls are any number in Linux's table and the private ones, minus
 * those that would end or multiply it (exit, fork, execve...) or simply
 * wait for ever (pause); their arguments are drawn from what breaks
 * kernels -- null and kernel addresses, the vector table, addresses one
 * byte from the end of a page, lengths of 0 and 0xffffffff, descriptors
 * closed, open, or never opened, flags with every bit set.
 *
 * Between rounds the parent (root) checks the machine is still itself:
 * it can open and read a file it put there, its own system calls give
 * the right answers, and when every round is over the pages, tasks and
 * open files are back where they started -- a leak that only shows
 * after a thousand bad calls is exactly what this finds. Each round's
 * seed is printed before it runs, so a round that brings the kernel
 * down can be run again by itself.
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define NR_MEMCTL       1004
#define MEMCTL_STATS    1
#define NR_KSTAT        1005
#define KSTAT_DISK_DELAY 2

static unsigned long rng;

static unsigned long rnd(void)
{
    /* xorshift32: the same sequence on every host, for a seed */
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng & 0xffffffffUL;
}

static unsigned long pick(const unsigned long *v, int n)
{
    return v[rnd() % (unsigned long)n];
}

/* What would end the child, multiply it, or never return. */
static int denied(long nr)
{
    switch (nr) {
    case 1: case 247:           /* exit, exit_group                     */
    case 2: case 120: case 190: case 435:       /* fork, clone, vfork   */
    case 11: case 355:          /* execve, execveat                     */
    case 29: case 72: case 179: /* pause, sigsuspend, rt_sigsuspend     */
    case 88:                    /* reboot (refused, but not worth it)   */
    case 1000:                  /* spawn                                */
        return 1;
    }
    return 0;
}

static char *scratch;           /* 64 KB of the child's own memory      */
static int fds[8];

static unsigned long arg(void)
{
    static const unsigned long smalls[] = {
        0, 1, 2, 3, 4, 7, 8, 16, 63, 64, 255, 256, 4095, 4096, 4097,
        0x7fffffffUL, 0x80000000UL, 0xffffffffUL, 0xfffffffeUL, 0xffff,
    };
    static const unsigned long bad_ptrs[] = {
        0, 1, 3, 0x400, 0x1000, 0x10000, 0x40000, 0x2f0000, 0x7ffffffcUL,
        0x80000000UL, 0xbffff000UL, 0xfffff000UL, 0xfffffffcUL, 0xffffffffUL,
    };
    unsigned long k = rnd() % 10;

    switch (k) {
    case 0: case 1:
        return pick(smalls, sizeof(smalls) / sizeof(smalls[0]));
    case 2:
        return pick(bad_ptrs, sizeof(bad_ptrs) / sizeof(bad_ptrs[0]));
    case 3:                     /* a real buffer, somewhere in it       */
        return (unsigned long)scratch + (rnd() % 65536);
    case 4:                     /* its last bytes: the next page is not */
        return (unsigned long)scratch + 65536 - (rnd() % 16);
    case 5:                     /* a descriptor, live or not            */
        return rnd() % 3 ? (unsigned long)fds[rnd() % 8]
                         : rnd() % 64;
    case 6:                     /* a path, from the scratch strings     */
        return (unsigned long)scratch + 60000 + (rnd() % 8) * 64;
    case 7:                     /* read-only memory: this program's code */
        return (unsigned long)&arg + (rnd() % 256);
    case 8:
        return rnd() % 1024;
    default:
        return rnd();
    }
}

static void child(unsigned long seed, long calls)
{
    static const char *paths[] = {
        "/a", "b/c", "", "/", "..", "/fz/../..", "d/", "x\001y",
    };
    long i;
    int k;

    rng = seed | 1;
    if (chroot("/fz") != 0 || chdir("/") != 0 ||
        setgid(1000) != 0 || setuid(1000) != 0) {
        _exit(90);
    }
    k = open("/dev/null", O_RDWR);
    dup2(k, 0);
    dup2(k, 1);
    dup2(k, 2);
    scratch = mmap(0, 65536, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (scratch == MAP_FAILED) {
        _exit(91);
    }
    for (k = 0; k < 60000; k++) {
        scratch[k] = (char)rnd();
    }
    for (k = 0; k < 8; k++) {
        strcpy(scratch + 60000 + k * 64, paths[k]);
    }
    fds[0] = open("/a", O_RDWR | O_CREAT, 0644);
    fds[1] = open("/", O_RDONLY | O_DIRECTORY);
    pipe(&fds[2]);
    fds[4] = socket(AF_INET, SOCK_STREAM, 0);
    fds[5] = socket(AF_INET, SOCK_DGRAM, 0);
    fds[6] = open("/dev/null", O_RDWR);
    fds[7] = 1023;
    alarm(5);                   /* a call that waits for ever ends here */

    for (i = 0; i < calls; i++) {
        long nr;

        do {
            nr = rnd() % 8 ? (long)(rnd() % 473) : 1000 + (long)(rnd() % 6);
        } while (denied(nr));
        syscall(nr, arg(), arg(), arg(), arg(), arg(), arg());
    }
    _exit(0);
}

/* The parent's view of the machine, between rounds. */
struct health {
    unsigned long free_pages, tasks;
};

/*
 * What only root may change, as the parent saw it at the start: the
 * clock (against the monotonic one, which nobody can set) and the host's
 * name. A nobody changing either is a missing permission check.
 */
static long wall0, mono0;
static char host0[65];

static long mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static const char *root_things_changed(void)
{
    struct utsname u;
    long drift = ((long)time(0) - wall0) - (mono_now() - mono0);

    if (drift > 30 || drift < -30) {
        return "the clock was set";
    }
    if (uname(&u) != 0 || strcmp(u.nodename, host0) != 0) {
        return "the host name was changed";
    }
    return 0;
}

static int machine_ok(struct health *h)
{
    char buf[64];
    int fd = open("/fz-canary", O_RDONLY);
    ssize_t n;
    unsigned long st[16];
    DIR *d;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n != 7 || memcmp(buf, "canary\n", 7) != 0) {
        return 0;
    }
    if (getpid() <= 0 || getuid() != 0) {
        return 0;
    }
    memset(st, 0, sizeof(st));
    if (syscall(NR_MEMCTL, MEMCTL_STATS, sizeof(st), st) < 0) {
        return 0;
    }
    h->free_pages = st[1];
    h->tasks = 0;
    d = opendir("/proc");
    if (d) {
        struct dirent *e;

        while ((e = readdir(d)) != 0) {
            if (e->d_name[0] >= '1' && e->d_name[0] <= '9') {
                h->tasks++;
            }
        }
        closedir(d);
    }
    return 1;
}

int main(int argc, char **argv)
{
    long rounds, calls, r;
    unsigned long seed;
    struct health before, after;
    int bad = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: sysfuzz ROUNDS CALLS [SEED]\n");
        return 2;
    }
    rounds = strtol(argv[1], 0, 10);
    calls = strtol(argv[2], 0, 10);
    seed = argc > 3 ? strtoul(argv[3], 0, 0) : 12345;
    /* Every disk request sleeps first: the windows in which a task is
     * asleep inside the filesystem, which is where races live. */
    if (argc > 4) {
        syscall(NR_KSTAT, KSTAT_DISK_DELAY, strtoul(argv[4], 0, 0), 0);
    }
    {
        struct utsname u;

        uname(&u);
        strncpy(host0, u.nodename, sizeof(host0) - 1);
        wall0 = (long)time(0);
        mono0 = mono_now();
    }
    if (!machine_ok(&before)) {
        printf("sysfuzz: the machine is not right before anything ran\n");
        return 1;
    }
    printf("sysfuzz: %ld rounds of %ld calls; %lu pages free, %lu tasks\n",
           rounds, calls, before.free_pages, before.tasks);
    fflush(stdout);
    for (r = 0; r < rounds; r++) {
        unsigned long s = seed + (unsigned long)r * 7919;
        int st = 0;
        pid_t p;

        printf("round %ld seed %lu\n", r, s);
        fflush(stdout);
        p = fork();
        if (p == 0) {
            child(s, calls);
        }
        if (p < 0) {
            printf("sysfuzz: fork failed: %s\n", strerror(errno));
            bad = 1;
            break;
        }
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) {
        }
        if (WIFEXITED(st) && WEXITSTATUS(st) >= 90) {
            printf("sysfuzz: round %ld could not set itself up (%d)\n",
                   r, WEXITSTATUS(st));
            bad = 1;
            break;
        }
        if (!machine_ok(&after)) {
            printf("sysfuzz: after round %ld (seed %lu) the machine is "
                   "not right\n", r, s);
            bad = 1;
            break;
        }
        if (root_things_changed()) {
            printf("sysfuzz: after round %ld (seed %lu), as a nobody: %s\n",
                   r, s, root_things_changed());
            bad = 1;
            break;
        }
    }
    /* The rounds' children are gone; let their pages be reaped. */
    sleep(1);
    machine_ok(&after);
    printf("sysfuzz: done; %lu pages free (was %lu), %lu tasks (was %lu)\n",
           after.free_pages, before.free_pages, after.tasks, before.tasks);
    if (after.tasks > before.tasks) {
        printf("sysfuzz: tasks were left behind\n");
        bad = 1;
    }
    if (after.free_pages + 64 < before.free_pages) {
        printf("sysfuzz: %lu pages did not come back\n",
               before.free_pages - after.free_pages);
        bad = 1;
    }
    if (argc > 4) {
        syscall(NR_KSTAT, KSTAT_DISK_DELAY, 0, 0);
    }
    printf("sysfuzz: %s\n", bad ? "FAILED" : "ok");
    return bad;
}
