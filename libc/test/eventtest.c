/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * eventtest - eventfd, timerfd, signalfd, epoll, inotify, ppoll, pselect.
 *
 * Where a check can lean on something other than the call it tests, it
 * does: a timer is timed against CLOCK_MONOTONIC, a wakeup is caused by
 * ANOTHER process, a signal read from a signalfd is checked gone from
 * sigpending(), an inotify event is checked against the change that
 * caused it. The one wait that covers all of them -- an epoll holding a
 * pipe, an eventfd, a timerfd and a signalfd -- is the point of the
 * whole feature, and has its own check.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    checks++;
    if (!ok) {
        failures++;
    }
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ms_sleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, 0);
}

/* A child that waits `ms` and then does `what`, and exits. */
static pid_t later(long ms, void (*what)(int), int arg)
{
    pid_t pid = fork();

    if (pid == 0) {
        ms_sleep(ms);
        what(arg);
        _exit(0);
    }
    return pid;
}

static void reap(pid_t pid)
{
    int st;

    waitpid(pid, &st, 0);
}

static int link_is(int fd, const char *want)
{
    char path[64], buf[64];
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) {
        return 0;
    }
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

/* ---------------------------------------------------------------- */

static int efd_for_child;

static void child_writes(int v)
{
    eventfd_write(efd_for_child, (eventfd_t)v);
}

static void eventfds(void)
{
    eventfd_t v = 0;
    uint32_t small;
    struct pollfd p;
    int fd, sfd;
    long t0;
    pid_t pid;

    fd = eventfd(3, EFD_NONBLOCK);
    check("eventfd: created, and its first read is the initial value",
          fd >= 0 && eventfd_read(fd, &v) == 0 && v == 3);
    check("  a read of zero is EAGAIN when non-blocking",
          read(fd, &v, 8) == -1 && errno == EAGAIN);
    eventfd_write(fd, 5);
    eventfd_write(fd, 7);
    check("  writes add up, and a read takes the lot",
          eventfd_read(fd, &v) == 0 && v == 12);
    check("  a buffer shorter than 8 bytes is EINVAL",
          read(fd, &small, 4) == -1 && errno == EINVAL);
    v = 0xffffffffffffffffULL;
    check("  writing 2^64-1 is EINVAL", write(fd, &v, 8) == -1 && errno == EINVAL);
    v = 0xfffffffffffffffeULL;
    write(fd, &v, 8);
    v = 1;
    check("  and a write that would pass 2^64-2 waits (EAGAIN here)",
          write(fd, &v, 8) == -1 && errno == EAGAIN);
    p.fd = fd;
    p.events = POLLIN | POLLOUT;
    check("  poll: full is readable and not writable",
          poll(&p, 1, 0) == 1 && p.revents == POLLIN);
    {
        int r1 = eventfd_read(fd, &v), r2 = poll(&p, 1, 0);

        check("  poll: empty is writable and not readable",
              r1 == 0 && r2 == 1 && (p.revents & POLLOUT) &&
              !(p.revents & POLLIN));
        if (!(r1 == 0 && r2 == 1 && (p.revents & POLLOUT) &&
              !(p.revents & POLLIN))) {
            printf("         (read %d, poll %d, revents %#x, v %#llx)\n", r1,
                   r2, p.revents, (unsigned long long)v);
        }
    }
    check("  /proc names it anon_inode:[eventfd]",
          link_is(fd, "anon_inode:[eventfd]"));
    close(fd);

    sfd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    check("EFD_SEMAPHORE: each read takes one",
          eventfd_read(sfd, &v) == 0 && v == 1 &&
          eventfd_read(sfd, &v) == 0 && v == 1 &&
          eventfd_read(sfd, &v) == -1 && errno == EAGAIN);
    close(sfd);

    efd_for_child = eventfd(0, 0);
    t0 = now_ms();
    pid = later(300, child_writes, 42);
    v = 0;
    check("a blocking read sleeps until ANOTHER process writes",
          eventfd_read(efd_for_child, &v) == 0 && v == 42 &&
          now_ms() - t0 >= 250);
    reap(pid);
    close(efd_for_child);
    check("eventfd flags other than its own are EINVAL",
          eventfd(0, 0x4) == -1 && errno == EINVAL);
}

/* ---------------------------------------------------------------- */

static void timerfds(void)
{
    struct itimerspec its, cur;
    struct pollfd p;
    uint64_t n = 0;
    long t0, dt;
    int fd;

    fd = timerfd_create(CLOCK_MONOTONIC, 0);
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 150 * 1000000L;
    t0 = now_ms();
    timerfd_settime(fd, 0, &its, 0);
    check("timerfd: a one-shot of 150 ms, read blocks till it fires, once",
          read(fd, &n, 8) == 8 && n == 1);
    dt = now_ms() - t0;
    /*
     * A blocked read sleeps in 100 ms slices; only the timer's own
     * deadline cutting the sleep short gets it back before 200 ms.
     */
    check("  and that took 150 ms by CLOCK_MONOTONIC (140..190)",
          dt >= 140 && dt < 190);
    if (!(dt >= 140 && dt < 190)) {
        printf("         (%ld ms)\n", dt);
    }
    check("  after which it is disarmed",
          timerfd_gettime(fd, &cur) == 0 && cur.it_value.tv_sec == 0 &&
          cur.it_value.tv_nsec == 0);

    its.it_interval.tv_nsec = 50 * 1000000L;
    its.it_value.tv_nsec = 50 * 1000000L;
    timerfd_settime(fd, 0, &its, 0);
    check("  gettime while armed: some time left, and the interval",
          timerfd_gettime(fd, &cur) == 0 && cur.it_value.tv_sec == 0 &&
          cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 50 * 1000000L &&
          cur.it_interval.tv_nsec == 50 * 1000000L);
    ms_sleep(330);
    check("  an interval of 50 ms, 330 ms later, has fired 5 to 7 times",
          read(fd, &n, 8) == 8 && n >= 5 && n <= 7);
    if (!(n >= 5 && n <= 7)) {
        printf("         (%llu)\n", (unsigned long long)n);
    }

    memset(&its, 0, sizeof(its));
    timerfd_settime(fd, 0, &its, 0);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    check("  a zero value disarms: nothing to read",
          read(fd, &n, 8) == -1 && errno == EAGAIN);

    /* (poll looks at a timerfd every 20 ms anyway, so this is a check
     * that poll sees it at all; the deadline is checked by read above.) */
    its.it_value.tv_nsec = 30 * 1000000L;
    timerfd_settime(fd, 0, &its, 0);
    p.fd = fd;
    p.events = POLLIN;
    t0 = now_ms();
    check("  poll wakes when the timer fires, not at the end of a slice",
          poll(&p, 1, 2000) == 1 && (p.revents & POLLIN) &&
          (dt = now_ms() - t0) < 90);
    if (dt >= 90) {
        printf("         (%ld ms)\n", dt);
    }
    read(fd, &n, 8);
    close(fd);

    fd = timerfd_create(CLOCK_REALTIME, 0);
    clock_gettime(CLOCK_REALTIME, &its.it_value);
    its.it_value.tv_nsec += 200 * 1000000L;
    if (its.it_value.tv_nsec >= 1000000000L) {
        its.it_value.tv_nsec -= 1000000000L;
        its.it_value.tv_sec++;
    }
    its.it_interval.tv_sec = its.it_interval.tv_nsec = 0;
    t0 = now_ms();
    check("TFD_TIMER_ABSTIME on CLOCK_REALTIME: now + 200 ms fires then",
          timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, 0) == 0 &&
          read(fd, &n, 8) == 8 && n == 1 &&
          (dt = now_ms() - t0) >= 150 && dt <= 450);
    its.it_value.tv_sec -= 100;
    check("  and a time already past fires at once",
          timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, 0) == 0 &&
          read(fd, &n, 8) == 8 && n == 1);
    its.it_value.tv_nsec = 1000000000L;
    check("  a nanosecond count of 10^9 is EINVAL",
          timerfd_settime(fd, 0, &its, 0) == -1 && errno == EINVAL);
    close(fd);
    check("an unknown clock is EINVAL",
          timerfd_create(99, 0) == -1 && errno == EINVAL);
}

/* ---------------------------------------------------------------- */

static void send_usr2(int pid)
{
    kill(pid, SIGUSR2);
}

static void signalfds(void)
{
    struct signalfd_siginfo si[2];
    sigset_t mask, pend;
    struct pollfd p;
    int fd;
    pid_t pid;
    long t0;

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, 0);

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    fd = signalfd(-1, &mask, SFD_NONBLOCK);
    check("signalfd: nothing pending reads EAGAIN",
          fd >= 0 && read(fd, si, sizeof(si[0])) == -1 && errno == EAGAIN);
    p.fd = fd;
    p.events = POLLIN;
    check("  and does not poll readable", poll(&p, 1, 0) == 0);
    kill(getpid(), SIGUSR1);
    check("  a blocked signal makes it poll readable",
          poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    sigpending(&pend);
    check("  (the signal is pending, by sigpending)", sigismember(&pend, SIGUSR1));
    check("  read gives it, 128 bytes, with its number",
          read(fd, si, sizeof(si)) == 128 && si[0].ssi_signo == SIGUSR1);
    sigpending(&pend);
    check("  and reading it took it: no longer pending",
          !sigismember(&pend, SIGUSR1));
    check("  a buffer shorter than a record is EINVAL",
          read(fd, si, 100) == -1 && errno == EINVAL);

    kill(getpid(), SIGUSR2);
    check("a signal not in its mask is not its to read",
          read(fd, si, sizeof(si)) == -1 && errno == EAGAIN);
    sigaddset(&mask, SIGUSR2);
    check("  signalfd(fd, ...) widens the mask of the same descriptor",
          signalfd(fd, &mask, 0) == fd &&
          read(fd, si, sizeof(si)) == 128 && si[0].ssi_signo == SIGUSR2);

    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR2);
    check("  two pending, one read of two records takes both",
          read(fd, si, sizeof(si)) == 256 && si[0].ssi_signo == SIGUSR1 &&
          si[1].ssi_signo == SIGUSR2);

    fcntl(fd, F_SETFL, 0);
    t0 = now_ms();
    pid = later(300, send_usr2, getpid());
    check("  a blocking read sleeps until another process sends one",
          read(fd, si, sizeof(si)) == 128 && si[0].ssi_signo == SIGUSR2 &&
          now_ms() - t0 >= 250);
    reap(pid);
    check("  /proc names it anon_inode:[signalfd]",
          link_is(fd, "anon_inode:[signalfd]"));
    close(fd);
    sigprocmask(SIG_UNBLOCK, &mask, 0);
}

/* ---------------------------------------------------------------- */

static volatile int got_usr1;

static void on_usr1(int sig)
{
    (void)sig;
    got_usr1++;
}

static void send_usr1(int pid)
{
    kill(pid, SIGUSR1);
}

static int ep_one(int ep, int timeout, struct epoll_event *out)
{
    return epoll_wait(ep, out, 1, timeout);
}

static void epolls(void)
{
    struct epoll_event ev, out[8];
    int ep, ep2, pfd[2], pfd2[2], efd, tfd, sfd, fd, n, i, seen;
    long dt;
    struct itimerspec its;
    sigset_t mask, now;
    char c;
    long t0;
    pid_t pid;

    check("epoll: struct epoll_event is Linux/m68k's 12 bytes",
          sizeof(struct epoll_event) == 12);
    ep = epoll_create1(EPOLL_CLOEXEC);
    check("epoll_create1", ep >= 0 && link_is(ep, "anon_inode:[eventpoll]"));
    check("  epoll_create(0) is EINVAL", epoll_create(0) == -1 && errno == EINVAL);

    pipe(pfd);
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ULL;
    check("  EPOLL_CTL_ADD of a pipe",
          epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) == 0);
    check("  added twice is EEXIST",
          epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) == -1 && errno == EEXIST);
    check("  nothing to read: nothing reported", ep_one(ep, 0, out) == 0);
    write(pfd[1], "x", 1);
    memset(out, 0, sizeof(out));
    check("  a byte written: reported, with its 64 bits of data intact",
          ep_one(ep, 0, out) == 1 && out[0].events == EPOLLIN &&
          out[0].data.u64 == 0x1122334455667788ULL);
    check("  level-triggered: reported again while unread",
          ep_one(ep, 0, out) == 1);

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = pfd[0];
    epoll_ctl(ep, EPOLL_CTL_MOD, pfd[0], &ev);
    check("EPOLLET: reported once for what is there",
          ep_one(ep, 0, out) == 1 && out[0].data.fd == pfd[0]);
    check("  and not again while nothing changes", ep_one(ep, 0, out) == 0);
    read(pfd[0], &c, 1);
    ep_one(ep, 0, out);
    write(pfd[1], "y", 1);
    check("  but again once it has been emptied and refilled",
          ep_one(ep, 0, out) == 1);
    read(pfd[0], &c, 1);

    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep, EPOLL_CTL_MOD, pfd[0], &ev);
    write(pfd[1], "z", 1);
    check("EPOLLONESHOT: reported once",
          ep_one(ep, 0, out) == 1 && ep_one(ep, 0, out) == 0);
    check("  and EPOLL_CTL_MOD arms it again",
          epoll_ctl(ep, EPOLL_CTL_MOD, pfd[0], &ev) == 0 &&
          ep_one(ep, 0, out) == 1);
    read(pfd[0], &c, 1);

    t0 = now_ms();
    check("a timeout of 200 ms on nothing returns 0 after 200 ms",
          ep_one(ep, 200, out) == 0 && now_ms() - t0 >= 180);

    fd = open("/evreg", O_CREAT | O_RDWR, 0644);
    check("a regular file is EPERM",
          epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == -1 && errno == EPERM);
    close(fd);
    unlink("/evreg");
    check("  adding the epoll to itself is EINVAL",
          epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) == -1 && errno == EINVAL);
    check("  EPOLL_CTL_DEL of something not in it is ENOENT",
          epoll_ctl(ep, EPOLL_CTL_DEL, pfd[1], 0) == -1 && errno == ENOENT);

    ep2 = epoll_create1(0);
    ev.events = EPOLLIN;
    check("an epoll inside an epoll",
          epoll_ctl(ep2, EPOLL_CTL_ADD, ep, &ev) == 0);
    check("  and the other way round as well is ELOOP",
          epoll_ctl(ep, EPOLL_CTL_ADD, ep2, &ev) == -1 && errno == ELOOP);
    ev.events = EPOLLIN;
    epoll_ctl(ep, EPOLL_CTL_MOD, pfd[0], &ev);
    write(pfd[1], "w", 1);
    check("  the outer one is readable when the inner one's pipe is",
          ep_one(ep2, 0, out) == 1);
    read(pfd[0], &c, 1);
    close(ep2);

    /* Closing the only descriptor for a file takes it out of the set. */
    close(pfd[0]);
    pipe(pfd2);
    check("closing a watched descriptor removes it: its number can be "
          "added again", pfd2[0] == pfd[0] &&
          epoll_ctl(ep, EPOLL_CTL_ADD, pfd2[0], &ev) == 0);
    epoll_ctl(ep, EPOLL_CTL_DEL, pfd2[0], 0);

    /* ONE wait for everything: a pipe, a counter, a timer, a signal. */
    efd = eventfd(0, EFD_NONBLOCK);
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, 0);
    sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    ev.events = EPOLLIN; ev.data.u32 = 1;
    epoll_ctl(ep, EPOLL_CTL_ADD, pfd2[0], &ev);
    ev.data.u32 = 2;
    epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
    ev.data.u32 = 3;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    ev.data.u32 = 4;
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 100 * 1000000L;
    timerfd_settime(tfd, 0, &its, 0);
    t0 = now_ms();
    n = epoll_wait(ep, out, 8, 3000);
    dt = now_ms() - t0;
    check("one epoll_wait over pipe, eventfd, timerfd, signalfd: the timer "
          "wakes it at 100 ms", n == 1 && out[0].data.u32 == 3 &&
          dt >= 90 && dt < 400);
    if (!(n == 1 && out[0].data.u32 == 3 && dt >= 90 && dt < 400)) {
        printf("         (n %d, %ld ms)\n", n, dt);
        for (i = 0; i < n; i++) {
            printf("         (%lu/%#lx)\n", (unsigned long)out[i].data.u32,
                   (unsigned long)out[i].events);
        }
    }
    {
        uint64_t x;

        read(tfd, &x, 8);
    }
    write(pfd2[1], "p", 1);
    eventfd_write(efd, 1);
    kill(getpid(), SIGUSR2);
    n = epoll_wait(ep, out, 8, 0);
    for (seen = 0, i = 0; i < n; i++) {
        seen |= 1 << out[i].data.u32;
    }
    check("  and then the other three together", n == 3 && seen == 0x16);
    if (!(n == 3 && seen == 0x16)) {
        printf("         (n %d, seen %#x)\n", n, seen);
        for (i = 0; i < n; i++) {
            printf("         (%lu/%#lx)\n", (unsigned long)out[i].data.u32,
                   (unsigned long)out[i].events);
        }
    }
    {
        struct signalfd_siginfo si;
        eventfd_t x;

        read(sfd, &si, sizeof(si));
        eventfd_read(efd, &x);
        read(pfd2[0], &c, 1);
    }
    n = epoll_wait(ep, out, 1, 0);
    check("  with max 1 and two ready, both are reported in turn",
          (write(pfd2[1], "q", 1), eventfd_write(efd, 1),
           epoll_wait(ep, out, 1, 0) == 1 &&
           (i = (int)out[0].data.u32, epoll_wait(ep, out, 1, 0) == 1) &&
           (int)out[0].data.u32 != i));
    (void)n;
    {
        eventfd_t x;

        read(pfd2[0], &c, 1);
        eventfd_read(efd, &x);
    }
    close(sfd);
    close(tfd);
    close(efd);
    sigprocmask(SIG_UNBLOCK, &mask, 0);

    /* epoll_pwait: a signal blocked outside the wait and let in during. */
    signal(SIGUSR1, on_usr1);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, 0);
    sigemptyset(&mask);
    got_usr1 = 0;
    pid = later(200, send_usr1, getpid());
    n = epoll_pwait(ep, out, 8, 3000, &mask);
    i = errno;
    check("epoll_pwait: a signal its mask lets through ends it with EINTR",
          n == -1 && i == EINTR && got_usr1 == 1);
    if (!(n == -1 && i == EINTR && got_usr1 == 1)) {
        printf("         (n %d, errno %d, handler ran %d times)\n", n, i,
               got_usr1);
    }
    reap(pid);
    sigprocmask(SIG_BLOCK, 0, &now);
    check("  and the old mask is back afterwards", sigismember(&now, SIGUSR1));

    got_usr1 = 0;
    pid = later(200, send_usr1, getpid());
    {
        struct pollfd p = { pfd2[0], POLLIN, 0 };
        struct timespec ts = { 3, 0 };

        n = ppoll(&p, 1, &ts, &mask);
    }
    check("ppoll: the same", n == -1 && errno == EINTR && got_usr1 == 1);
    reap(pid);
    sigprocmask(SIG_BLOCK, 0, &now);
    check("  and the old mask is back afterwards", sigismember(&now, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &now, 0);
    signal(SIGUSR1, SIG_DFL);

    {
        struct pollfd p = { pfd2[0], POLLIN, 0 };
        struct timespec ts = { 0, 150 * 1000000L };

        t0 = now_ms();
        check("ppoll: a 150 ms timeout", ppoll(&p, 1, &ts, 0) == 0 &&
              now_ms() - t0 >= 140);
    }
    {
        fd_set rd;
        struct timespec ts = { 0, 0 };

        write(pfd2[1], "r", 1);
        FD_ZERO(&rd);
        FD_SET(pfd2[0], &rd);
        check("pselect: a readable pipe is reported",
              pselect(pfd2[0] + 1, &rd, 0, 0, &ts, 0) == 1 &&
              FD_ISSET(pfd2[0], &rd));
    }
    close(pfd2[0]);
    close(pfd2[1]);
    close(pfd[1]);
    close(ep);
}

/* ---------------------------------------------------------------- */

struct iev {
    int wd;
    uint32_t mask, cookie;
    char name[64];
};

/* Everything queued, as a list. */
static int drain(int fd, struct iev *out, int max)
{
    char buf[2048];
    int n = 0;
    ssize_t got;

    while ((got = read(fd, buf, sizeof(buf))) > 0) {
        char *p = buf;

        while (p < buf + got && n < max) {
            struct inotify_event *e = (struct inotify_event *)p;

            out[n].wd = e->wd;
            out[n].mask = e->mask;
            out[n].cookie = e->cookie;
            out[n].name[0] = '\0';
            if (e->len) {
                snprintf(out[n].name, sizeof(out[n].name), "%s", e->name);
            }
            n++;
            p += sizeof(*e) + e->len;
        }
    }
    return n;
}

static int has(struct iev *v, int n, uint32_t mask, const char *name)
{
    int i;

    for (i = 0; i < n; i++) {
        if (v[i].mask == mask && strcmp(v[i].name, name) == 0) {
            return i + 1;
        }
    }
    return 0;
}

static void print_events(struct iev *v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        printf("         wd %d mask %#lx cookie %lu '%s'\n", v[i].wd,
               (unsigned long)v[i].mask, (unsigned long)v[i].cookie,
               v[i].name);
    }
}

static void touch_child(int unused)
{
    int fd = open("/evdir/h", O_WRONLY | O_APPEND);

    (void)unused;
    write(fd, "c", 1);
    close(fd);
}

static void inotify_in(const char *dir)
{
    char path[128], path2[128];
    struct iev v[200];
    int in, wd, fd, n, a, b, ok;
    struct pollfd p;

    printf("  -- inotify in %s\n", dir);
    mkdir(dir, 0755);
    in = inotify_init1(IN_NONBLOCK);
    wd = inotify_add_watch(in, dir, IN_ALL_EVENTS);
    check("inotify: a watch on a directory", in >= 0 && wd > 0);
    check("  nothing yet: EAGAIN", read(in, v, sizeof(v)) == -1 && errno == EAGAIN);

    snprintf(path, sizeof(path), "%s/f", dir);
    fd = open(path, O_CREAT | O_WRONLY, 0644);
    write(fd, "hello", 5);
    close(fd);
    p.fd = in;
    p.events = POLLIN;
    check("  a change makes it poll readable", poll(&p, 1, 0) == 1);
    n = drain(in, v, 200);
    a = has(v, n, IN_CREATE, "f");
    ok = a && has(v, n, IN_OPEN, "f") > a && has(v, n, IN_MODIFY, "f") > a &&
         has(v, n, IN_CLOSE_WRITE, "f") > has(v, n, IN_MODIFY, "f") &&
         v[0].wd == wd;
    check("  create, write, close: CREATE, OPEN, MODIFY, CLOSE_WRITE, in order",
          ok);
    if (!ok) {
        print_events(v, n);
    }

    fd = open(path, O_RDONLY);
    close(fd);
    n = drain(in, v, 200);
    check("  open and close for reading: OPEN, CLOSE_NOWRITE",
          n == 2 && has(v, n, IN_OPEN, "f") == 1 &&
          has(v, n, IN_CLOSE_NOWRITE, "f") == 2);

    snprintf(path2, sizeof(path2), "%s/sub", dir);
    mkdir(path2, 0755);
    n = drain(in, v, 200);
    check("  mkdir: CREATE with IN_ISDIR", has(v, n, IN_CREATE | IN_ISDIR, "sub"));
    rmdir(path2);
    n = drain(in, v, 200);
    check("  rmdir: DELETE with IN_ISDIR", has(v, n, IN_DELETE | IN_ISDIR, "sub"));

    snprintf(path2, sizeof(path2), "%s/g", dir);
    rename(path, path2);
    n = drain(in, v, 200);
    a = has(v, n, IN_MOVED_FROM, "f");
    b = has(v, n, IN_MOVED_TO, "g");
    check("  rename: MOVED_FROM f and MOVED_TO g, sharing a cookie",
          a && b && v[a - 1].cookie != 0 && v[a - 1].cookie == v[b - 1].cookie);

    chmod(path2, 0600);
    n = drain(in, v, 200);
    check("  chmod: ATTRIB", has(v, n, IN_ATTRIB, "g"));

    unlink(path2);
    n = drain(in, v, 200);
    check("  unlink: DELETE", has(v, n, IN_DELETE, "g"));

    /* The padding: a name's record is a multiple of 16, NUL-filled. */
    {
        char buf[256];
        struct inotify_event *e = (struct inotify_event *)buf;

        snprintf(path, sizeof(path), "%s/abcde", dir);
        fd = open(path, O_CREAT | O_WRONLY, 0644);
        n = (int)read(in, buf, sizeof(buf));
        check("  a record's name is NUL-padded to a multiple of 16",
              n >= (int)sizeof(*e) + 16 && e->len == 16 &&
              strcmp(e->name, "abcde") == 0 && e->name[15] == '\0');
        close(fd);
        unlink(path);
        drain(in, v, 200);
    }

    check("inotify_rm_watch, and IN_IGNORED for it",
          inotify_rm_watch(in, wd) == 0 && drain(in, v, 200) == 1 &&
          v[0].mask == IN_IGNORED && v[0].wd == wd);
    check("  rm_watch of a watch that is not there is EINVAL",
          inotify_rm_watch(in, wd) == -1 && errno == EINVAL);
    close(in);
}

static void inotifies(void)
{
    struct iev v[200];
    int in, wd, fwd, fd, n, i, ok;
    char path[64];
    pid_t pid;

    inotify_in("/evdir");
    inotify_in("/tmp/evdir");

    in = inotify_init1(IN_NONBLOCK);
    fd = open("/evdir/h", O_CREAT | O_WRONLY, 0644);
    close(fd);
    check("IN_ONLYDIR on a file is ENOTDIR",
          inotify_add_watch(in, "/evdir/h", IN_ALL_EVENTS | IN_ONLYDIR) == -1 &&
          errno == ENOTDIR);
    check("  a watch on nothing is ENOENT",
          inotify_add_watch(in, "/evdir/nothere", IN_ALL_EVENTS) == -1 &&
          errno == ENOENT);
    check("  an empty mask is EINVAL",
          inotify_add_watch(in, "/evdir/h", 0) == -1 && errno == EINVAL);

    fwd = inotify_add_watch(in, "/evdir/h", IN_MODIFY | IN_DELETE_SELF |
                            IN_ATTRIB);
    wd = inotify_add_watch(in, "/evdir", IN_MODIFY);
    check("  a second watch on the same inode is the same wd",
          inotify_add_watch(in, "/evdir/h", IN_MODIFY | IN_DELETE_SELF) == fwd &&
          fwd != wd);
    inotify_add_watch(in, "/evdir/h", IN_MODIFY | IN_DELETE_SELF | IN_ATTRIB);
    pid = later(0, touch_child, 0);
    reap(pid);
    n = drain(in, v, 200);
    ok = 0;
    for (i = 0; i < n; i++) {
        if (v[i].wd == fwd && v[i].mask == IN_MODIFY && !v[i].name[0]) {
            ok |= 1;
        }
        if (v[i].wd == wd && v[i].mask == IN_MODIFY && !strcmp(v[i].name, "h")) {
            ok |= 2;
        }
    }
    check("a watch on a file sees another process write it, nameless; "
          "its directory's watch sees it by name", ok == 3);
    if (ok != 3) {
        print_events(v, n);
    }
    unlink("/evdir/h");
    n = drain(in, v, 200);
    check("  unlink: the file's watch gets DELETE_SELF, then IGNORED",
          n == 2 && v[0].wd == fwd && v[0].mask == IN_DELETE_SELF &&
          v[1].wd == fwd && v[1].mask == IN_IGNORED);
    inotify_rm_watch(in, wd);
    drain(in, v, 200);

    /* A queue that fills ends in one IN_Q_OVERFLOW. */
    wd = inotify_add_watch(in, "/tmp/evdir", IN_CREATE);
    for (i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), "/tmp/evdir/n%03d", i);
        close(open(path, O_CREAT | O_WRONLY, 0644));
    }
    n = drain(in, v, 200);
    check("  200 creates overflow the queue: fewer events, the last "
          "IN_Q_OVERFLOW", n > 50 && n < 200 && v[n - 1].mask == IN_Q_OVERFLOW &&
          v[n - 1].wd == -1);
    for (i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), "/tmp/evdir/n%03d", i);
        unlink(path);
    }
    drain(in, v, 200);
    check("  and the queue works again once read",
          (close(open("/tmp/evdir/again", O_CREAT | O_WRONLY, 0644)),
           drain(in, v, 200) == 1 && has(v, 1, IN_CREATE, "again")));
    close(in);
    check("/proc names it anon_inode:inotify",
          (in = inotify_init()) >= 0 && link_is(in, "anon_inode:inotify"));
    close(in);
}

int main(void)
{
    eventfds();
    timerfds();
    signalfds();
    epolls();
    inotifies();
    printf("eventtest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
