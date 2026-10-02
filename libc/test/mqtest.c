/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mqtest - POSIX message queues (kernel/mqueue.c), through the raw
 * system calls: picolibc has no mq_* yet (todo: it needs <mqueue.h>).
 * The name goes in without its slash, as Linux's call takes it.
 *
 * Priority order (highest first, FIFO within one), the size and
 * priority limits, O_NONBLOCK, an absolute timeout that really waits,
 * a receiver and a sender blocked in another process and woken, the
 * notification signal with its code and value (and spent after one),
 * one registrant only, unlink while open, permissions, poll, the time64
 * calls, and a message that spans two pages arriving intact.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NR_mq_open           271
#define NR_mq_unlink         272
#define NR_mq_timedsend      273
#define NR_mq_timedreceive   274
#define NR_mq_notify         275
#define NR_mq_getsetattr     276
#define NR_mq_timedreceive64 419

#define LINUX_SIGUSR1 10        /* picolibc's SIGUSR1 is another number  */

/* And its O_ flags are newlib's, not Linux's (O_EXCL is Linux's
 * O_NONBLOCK): picolibc's open() translates, and a raw system call
 * has to say Linux's own. */
#define L_RDONLY   0
#define L_RDWR     2
#define L_CREAT    0x40
#define L_EXCL     0x80
#define L_NONBLOCK 0x800
#define SI_MESGQ_     (-3)

struct kattr {
    long flags, maxmsg, msgsize, curmsgs, reserved[4];
};

struct ksigevent {
    int value, signo, notify, pad[13];
};

static int checks, failures;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failures++;
    }
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static int mqopen(const char *name, int flags, int mode, struct kattr *a)
{
    return (int)syscall(NR_mq_open, name, flags, mode, a);
}

static long msend(int fd, const void *buf, size_t len, unsigned prio)
{
    return syscall(NR_mq_timedsend, fd, buf, len, prio, 0);
}

static long mrecv(int fd, void *buf, size_t len, unsigned *prio)
{
    return syscall(NR_mq_timedreceive, fd, buf, len, prio, 0);
}

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static volatile int got_sig, got_code, got_value;

static void on_usr1(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    got_sig++;
    got_code = si->si_code;
    got_value = si->si_value.sival_int;
}

static int fails_with(long r, int e)
{
    return r == -1 && errno == e;
}

static char big[8192], back[8192];

int main(void)
{
    struct kattr a, small = { 0, 2, 64, 0, { 0 } };
    unsigned prio;
    char buf[8192];
    int q, q2, i, st;
    long r, t0;
    pid_t child;

    printf("mqtest: start\n");
    syscall(NR_mq_unlink, "mqtest");
    syscall(NR_mq_unlink, "mqsmall");

    q = mqopen("mqtest", L_RDWR | L_CREAT | L_EXCL, 0600, 0);
    check("mq_open creates a queue", q >= 0);
    memset(&a, 0, sizeof(a));
    check("  with Linux's default attributes: 10 messages of 8192 bytes",
          syscall(NR_mq_getsetattr, q, 0, &a) == 0 && a.maxmsg == 10 &&
          a.msgsize == 8192 && a.curmsgs == 0);

    msend(q, "one", 3, 1);
    msend(q, "fiveA", 5, 5);
    msend(q, "three", 5, 3);
    msend(q, "fiveB", 5, 5);
    syscall(NR_mq_getsetattr, q, 0, &a);
    {
        char got[4][8];
        unsigned pr[4];
        int ok = a.curmsgs == 4;

        for (i = 0; i < 4; i++) {
            memset(got[i], 0, sizeof(got[i]));
            r = mrecv(q, buf, sizeof(buf), &pr[i]);
            memcpy(got[i], buf, r > 0 && r < 8 ? (size_t)r : 0);
        }
        ok = ok && strcmp(got[0], "fiveA") == 0 && pr[0] == 5 &&
             strcmp(got[1], "fiveB") == 0 && pr[1] == 5 &&
             strcmp(got[2], "three") == 0 && pr[2] == 3 &&
             strcmp(got[3], "one") == 0 && pr[3] == 1;
        check("received highest priority first, FIFO within one", ok);
    }

    check("a receive buffer smaller than mq_msgsize: EMSGSIZE",
          fails_with(mrecv(q, buf, 100, 0), EMSGSIZE));
    check("a message larger than mq_msgsize: EMSGSIZE",
          fails_with(msend(q, big, 8193, 0), EMSGSIZE));
    check("a priority of MQ_PRIO_MAX: EINVAL",
          fails_with(msend(q, "x", 1, 32768), EINVAL));

    memset(big, 0, sizeof(big));
    for (i = 0; i < (int)sizeof(big); i++) {
        big[i] = (char)(i * 7 + 3);
    }
    r = msend(q, big, sizeof(big), 0) == 0 ? mrecv(q, back, sizeof(back), 0) : -1;
    check("an 8192-byte message -- two pages -- arrives intact",
          r == 8192 && memcmp(big, back, sizeof(big)) == 0);

    q2 = mqopen("mqsmall", L_RDWR | L_CREAT | L_NONBLOCK, 0600, &small);
    check("O_NONBLOCK: an empty queue says EAGAIN",
          q2 >= 0 && fails_with(mrecv(q2, buf, 64, 0), EAGAIN));
    msend(q2, "a", 1, 0);
    msend(q2, "b", 1, 0);
    check("  and a full one (mq_maxmsg 2) says EAGAIN too",
          fails_with(msend(q2, "c", 1, 0), EAGAIN));
    {
        struct pollfd p = { q2, POLLIN | POLLOUT, 0 };
        int full_in, full_out;

        poll(&p, 1, 0);
        full_in = (p.revents & POLLIN) != 0;
        full_out = (p.revents & POLLOUT) != 0;
        mrecv(q2, buf, 64, 0);
        mrecv(q2, buf, 64, 0);
        p.revents = 0;
        poll(&p, 1, 0);
        check("poll: readable and not writable when full; the reverse empty",
              full_in && !full_out && !(p.revents & POLLIN) &&
              (p.revents & POLLOUT));
    }

    /* Both timespec layouts, spelled out: the old calls take Linux's
     * 32-bit one, the _time64 calls the 64-bit one -- whatever
     * picolibc's own struct timespec is. Each must really wait. */
    {
        struct timespec now;
        struct { unsigned long sec; long nsec; } t32;
        struct { long long sec; long pad; long nsec; } t64;
        long e32, e64;

        clock_gettime(CLOCK_REALTIME, &now);
        t32.sec = (unsigned long)now.tv_sec;
        t32.nsec = now.tv_nsec + 400000000L;
        if (t32.nsec >= 1000000000L) {
            t32.sec++;
            t32.nsec -= 1000000000L;
        }
        t0 = now_ms();
        r = syscall(NR_mq_timedreceive, q, buf, sizeof(buf), 0, &t32);
        e32 = now_ms() - t0;
        check("an absolute timeout 400 ms ahead: ETIMEDOUT, after ~400 ms",
              fails_with(r, ETIMEDOUT) && e32 >= 300 && e32 < 3000);

        t64.sec = (long long)t32.sec + 1;       /* the same, a second on */
        t64.pad = 0;
        t64.nsec = t32.nsec;
        t0 = now_ms();
        r = syscall(NR_mq_timedreceive64, q, buf, sizeof(buf), 0, &t64);
        e64 = now_ms() - t0;
        check("  and mq_timedreceive_time64 likewise",
              fails_with(r, ETIMEDOUT) && e64 >= 300 && e64 < 4000);
        if (e32 < 300 || e64 < 300) {
            printf("    waited %ld and %ld ms\n", e32, e64);
        }
        t64.sec = (long long)(unsigned long)now.tv_sec - 1;
        t64.nsec = 0;
        check("  and with a time already past: ETIMEDOUT at once",
              fails_with(syscall(NR_mq_timedreceive64, q, buf, sizeof(buf), 0,
                                 &t64), ETIMEDOUT));
    }

    /* A receiver blocked in another process, woken by the send. */
    child = fork();
    if (child == 0) {
        r = mrecv(q, buf, sizeof(buf), &prio);
        _exit(r == 5 && memcmp(buf, "hello", 5) == 0 && prio == 9 ? 0 : 1);
    }
    usleep(300000);
    msend(q, "hello", 5, 9);
    st = -1;
    waitpid(child, &st, 0);
    check("a receiver blocked in another process is woken by a send",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* And a sender blocked on a full queue, woken by a receive. */
    for (i = 0; i < 10; i++) {
        msend(q, "f", 1, 0);
    }
    child = fork();
    if (child == 0) {
        _exit(msend(q, "late", 4, 0) == 0 ? 0 : 1);
    }
    usleep(300000);
    syscall(NR_mq_getsetattr, q, 0, &a);
    i = (int)a.curmsgs;
    mrecv(q, buf, sizeof(buf), 0);
    st = -1;
    waitpid(child, &st, 0);
    syscall(NR_mq_getsetattr, q, 0, &a);
    check("a sender blocked on a full queue is woken by a receive",
          i == 10 && WIFEXITED(st) && WEXITSTATUS(st) == 0 && a.curmsgs == 10);
    while (a.curmsgs > 0) {
        mrecv(q, buf, sizeof(buf), 0);
        syscall(NR_mq_getsetattr, q, 0, &a);
    }

    /* mq_notify: a signal when the empty queue gets a message. */
    {
        struct sigaction sa;
        struct ksigevent ev;
        int first;

        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_usr1;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGUSR1, &sa, 0);
        memset(&ev, 0, sizeof(ev));
        ev.notify = 0;                  /* SIGEV_SIGNAL */
        ev.signo = LINUX_SIGUSR1;
        ev.value = 77;
        r = syscall(NR_mq_notify, q, &ev);
        child = fork();
        if (child == 0) {
            _exit(fails_with(syscall(NR_mq_notify, q, &ev), EBUSY) ? 0 : 1);
        }
        waitpid(child, &st, 0);
        check("mq_notify registers; another process then gets EBUSY",
              r == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        msend(q, "n", 1, 0);
        usleep(100000);
        first = got_sig;
        check("  a message into the empty queue raises the signal: SI_MESGQ, value 77",
              first == 1 && got_code == SI_MESGQ_ && got_value == 77);
        mrecv(q, buf, sizeof(buf), 0);
        msend(q, "n", 1, 0);
        usleep(100000);
        check("  and the registration is spent: no second signal",
              got_sig == 1);
        mrecv(q, buf, sizeof(buf), 0);
    }

    check("O_CREAT|O_EXCL on an existing name: EEXIST",
          fails_with(mqopen("mqtest", L_RDWR | L_CREAT | L_EXCL, 0600, 0), EEXIST));
    check("opening a name that does not exist: ENOENT",
          fails_with(mqopen("nosuchq", L_RDWR, 0, 0), ENOENT));
    check("a name with a slash in it: EACCES, as Linux's mqueue fs says",
          fails_with(mqopen("a/b", L_RDWR | L_CREAT, 0600, 0), EACCES));

    child = fork();
    if (child == 0) {
        if (setuid(1000) != 0) {
            _exit(2);
        }
        _exit(fails_with(mqopen("mqtest", L_RDONLY, 0, 0), EACCES) &&
              fails_with(syscall(NR_mq_unlink, "mqtest"), EACCES) ? 0 : 1);
    }
    waitpid(child, &st, 0);
    check("another user may not open a 0600 queue, or unlink it",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    check("mq_unlink removes the name",
          syscall(NR_mq_unlink, "mqtest") == 0 &&
          fails_with(mqopen("mqtest", L_RDWR, 0, 0), ENOENT));
    check("  but the open queue still works",
          msend(q, "still", 5, 2) == 0 && mrecv(q, buf, sizeof(buf), &prio) == 5 &&
          memcmp(buf, "still", 5) == 0 && prio == 2);
    close(q);
    q = mqopen("mqtest", L_RDWR | L_CREAT | L_EXCL, 0600, 0);
    check("  and after the last close the name can be made again, empty",
          q >= 0 && syscall(NR_mq_getsetattr, q, 0, &a) == 0 && a.curmsgs == 0);
    close(q);
    close(q2);
    syscall(NR_mq_unlink, "mqtest");
    syscall(NR_mq_unlink, "mqsmall");

    printf("mqtest: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
