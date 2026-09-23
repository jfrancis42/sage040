/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * timetest - interval timers, the clock, and accounting.
 *
 *   timetest           the checks
 *   timetest burn MS   use MS of processor time, then exit
 *   timetest alarm     let an alarm go off with no handler
 */
#include "ulib.h"

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static volatile int fired[NSIG];

static void count(int sig)
{
    fired[sig]++;
}

static u32 now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, 0);
    return (u32)tv.tv_sec * 1000 + (u32)tv.tv_usec / 1000;
}

/*
 * Use about `ms` of processor time, in user mode. The clock is looked at
 * rarely: a system call every few microseconds would make this mostly
 * SYSTEM time, which is exactly what the accounting check would catch.
 */
static void burn(u32 ms)
{
    struct tms t;
    u32 until;

    times(&t);
    until = t.tms_utime + (ms * HZ) / 1000;
    do {
        volatile u32 i;

        for (i = 0; i < 100000; i++) {
        }
        times(&t);
    } while ((s32)(t.tms_utime - until) < 0);
}

static void set(int which, u32 value_ms, u32 interval_ms)
{
    struct itimerval it;

    it.it_value.tv_sec = (s32)(value_ms / 1000);
    it.it_value.tv_usec = (s32)((value_ms % 1000) * 1000);
    it.it_interval.tv_sec = (s32)(interval_ms / 1000);
    it.it_interval.tv_usec = (s32)((interval_ms % 1000) * 1000);
    setitimer(which, &it, 0);
}

static void test_clock(void)
{
    struct timeval a, b;
    struct timespec ts;
    u32 i, t0, took;
    int mono = 1, sane = 1;
    time_t now;

    gettimeofday(&a, 0);
    for (i = 0; i < 2000; i++) {
        gettimeofday(&b, 0);
        if (b.tv_usec < 0 || b.tv_usec >= 1000000) {
            sane = 0;
        }
        if (b.tv_sec < a.tv_sec ||
            (b.tv_sec == a.tv_sec && b.tv_usec < a.tv_usec)) {
            mono = 0;
        }
        a = b;
    }
    report("gettimeofday never goes backwards", mono);
    report("  and its microseconds are always in range", sane);

    now = time(0);
    gettimeofday(&a, 0);
    report("time() and gettimeofday() agree on the second",
           a.tv_sec - (s32)now >= 0 && a.tv_sec - (s32)now <= 1);
    report("  and it is some time this century", a.tv_sec > 946684800);

    ts.tv_sec = 0;
    ts.tv_nsec = 300000000;
    t0 = now_ms();
    nanosleep(&ts, 0);
    took = now_ms() - t0;
    report("gettimeofday measures a 300 ms sleep as about 300 ms",
           took >= 290 && took <= 400);

    /* Setting the clock moves both, and it can be put back. */
    gettimeofday(&a, 0);
    b = a;
    b.tv_sec += 3600;
    report("settimeofday moves the clock an hour",
           settimeofday(&b, 0) == 0 && time(0) - (s32)a.tv_sec >= 3600);
    settimeofday(&a, 0);
    report("  and back", time(0) - (s32)a.tv_sec < 5);
    b.tv_usec = 1000000;
    report("  and refuses a microsecond count that is not one",
           settimeofday(&b, 0) == -EINVAL);
}

static void test_timers(void)
{
    struct itimerval it;
    struct timespec ts;
    u32 t0, took;

    /* alarm */
    signal(SIGALRM, count);
    fired[SIGALRM] = 0;
    t0 = now_ms();
    alarm(1);
    pause();
    took = now_ms() - t0;
    report("alarm(1) raises SIGALRM after a second",
           fired[SIGALRM] == 1 && took >= 990 && took < 1300);
    alarm(5);
    report("alarm returns what was left of the last one",
           alarm(0) == 5);
    report("  and alarm(0) cancels it", alarm(0) == 0);

    /* A repeating real timer. */
    fired[SIGALRM] = 0;
    t0 = now_ms();
    set(ITIMER_REAL, 100, 100);
    while (fired[SIGALRM] < 5) {
        pause();
    }
    took = now_ms() - t0;
    getitimer(ITIMER_REAL, &it);
    report("a 100 ms repeating timer fires five times in about 500 ms",
           took >= 490 && took < 700);
    report("  and getitimer reports its interval",
           it.it_interval.tv_sec == 0 && it.it_interval.tv_usec == 100000);
    report("  and a value no more than that",
           it.it_value.tv_sec == 0 && it.it_value.tv_usec <= 100000);
    set(ITIMER_REAL, 0, 0);
    getitimer(ITIMER_REAL, &it);
    report("a timer set to zero is off",
           it.it_value.tv_sec == 0 && it.it_value.tv_usec == 0);
    fired[SIGALRM] = 0;
    ts.tv_sec = 0;
    ts.tv_nsec = 300000000;
    nanosleep(&ts, 0);
    report("  and stays off", fired[SIGALRM] == 0);

    /* Virtual time passes only while this program runs in user mode. */
    signal(SIGVTALRM, count);
    fired[SIGVTALRM] = 0;
    set(ITIMER_VIRTUAL, 200, 0);
    ts.tv_sec = 0;
    ts.tv_nsec = 500000000;
    nanosleep(&ts, 0);
    report("ITIMER_VIRTUAL does not run while the program sleeps",
           fired[SIGVTALRM] == 0);
    t0 = now_ms();
    while (!fired[SIGVTALRM]) {
        volatile u32 i;

        for (i = 0; i < 1000; i++) {
        }
    }
    took = now_ms() - t0;
    report("  and fires after 200 ms of computing",
           took >= 190 && took < 1000);

    signal(SIGPROF, count);
    fired[SIGPROF] = 0;
    set(ITIMER_PROF, 100, 0);
    burn(300);
    report("ITIMER_PROF fires while the program runs",
           fired[SIGPROF] == 1);

    it.it_value.tv_sec = 0;
    it.it_value.tv_usec = 2000000;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 0;
    report("setitimer refuses a microsecond count that is not one",
           setitimer(ITIMER_REAL, &it, 0) == -EINVAL);
    report("  and a timer that does not exist",
           getitimer(7, &it) == -EINVAL);
}

static void test_accounting(void)
{
    struct tms a, b;
    static char ms[] = "300";
    char *argv[3];
    int child, st;

    times(&a);
    burn(200);
    times(&b);
    report("200 ms of computing is charged as user time",
           b.tms_utime - a.tms_utime >= 19 && b.tms_utime - a.tms_utime <= 30);
    report("  and hardly any of it as system time",
           b.tms_stime - a.tms_stime < 5);

    argv[0] = "/timetest";
    argv[1] = "burn";
    argv[2] = ms;
    child = spawn("/timetest", 3, argv, 0);
    waitpid(child, &st, 0);
    times(&b);
    report("a waited-for child's 300 ms arrive in cutime",
           b.tms_cutime - a.tms_cutime >= 29 && b.tms_cutime - a.tms_cutime <= 40);
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "burn") == 0) {
        u32 ms = 0;
        const char *p;

        for (p = argv[2]; *p >= '0' && *p <= '9'; p++) {
            ms = ms * 10 + (u32)(*p - '0');
        }
        burn(ms);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "alarm") == 0) {
        puts("timetest: waiting for an alarm nobody catches\n");
        alarm(1);
        pause();
        puts("ALARM DID NOT END THE PROGRAM\n");
        return 1;
    }
    test_clock();
    test_timers();
    test_accounting();
    puts("timetest: done\n");
    return 0;
}
