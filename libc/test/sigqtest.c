/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sigqtest - who may signal whom, and what the signal says about who
 * sent it. Run by kernel/procpstest.sh.
 *
 *   - sigqueue's value reaches a SA_SIGINFO handler and sigwaitinfo,
 *     with si_code SI_QUEUE and the sender's pid;
 *   - kill(2)'s signal says SI_USER and which process sent it;
 *   - a process that is not root may not signal one of root's -- by
 *     kill, by kill(-1), by tgkill or by sigqueue -- nor even ask
 *     whether it exists, and root's process receives nothing;
 *   - nobody but the process itself may claim a signal came from
 *     kill(2) (rt_sigqueueinfo with a non-negative si_code).
 *
 * The facts are cross-checked: a handler's si_pid against the pid fork()
 * returned, a count of signals received against the calls that should
 * and should not have delivered one.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SI_QUEUE_ (-1)

static int checks, failures;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failures++;
    }
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

static volatile int got_code, got_pid, got_val, got_uid, usr2_count;

static void on_usr1(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)uc;
    got_code = si->si_code;
    got_pid = si->si_pid;
    got_uid = (int)si->si_uid;
    got_val = si->si_value.sival_int;
}

static void on_usr2(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)uc;
    usr2_count++;
    got_code = si->si_code;
    got_pid = si->si_pid;
}

int main(void)
{
    struct sigaction sa;
    union sigval v;
    sigset_t set;
    siginfo_t info;
    pid_t me = getpid(), child;
    int st;

    printf("sigqtest: start\n");
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    sa.sa_sigaction = on_usr2;
    sigaction(SIGUSR2, &sa, 0);

    /* --- sigqueue to itself, through a handler ---------------------- */
    v.sival_int = 4242;
    check("sigqueue to itself succeeds", sigqueue(me, SIGUSR1, v) == 0);
    check("  the handler sees SI_QUEUE, the value, and its own pid",
          got_code == SI_QUEUE_ && got_val == 4242 && got_pid == me);

    /* --- and through sigwaitinfo ------------------------------------ */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, 0);
    v.sival_int = 7;
    sigqueue(me, SIGUSR1, v);
    memset(&info, 0, sizeof(info));
    check("sigwaitinfo takes a queued signal with its value",
          sigwaitinfo(&set, &info) == SIGUSR1 && info.si_code == SI_QUEUE_ &&
          info.si_value.sival_int == 7 && info.si_pid == me);
    sigprocmask(SIG_UNBLOCK, &set, 0);

    /* --- kill(2) from another process says who --------------------- */
    got_code = 99;
    child = fork();
    if (child == 0) {
        kill(getppid(), SIGUSR2);
        _exit(0);
    }
    waitpid(child, &st, 0);
    check("kill(2)'s signal: SI_USER, and the pid of the process that sent it",
          usr2_count == 1 && got_code == SI_USER && got_pid == child);

    /* --- a nobody may not signal root's process --------------------- */
    usr2_count = 0;
    child = fork();
    if (child == 0) {
        int bad = 0;
        pid_t root_pid = getppid();

        if (setgid(1000) != 0 || setuid(1000) != 0) {
            _exit(90);
        }
        errno = 0;
        if (!(kill(root_pid, SIGUSR2) < 0 && errno == EPERM)) bad |= 1;
        errno = 0;
        if (!(kill(root_pid, 0) < 0 && errno == EPERM)) bad |= 2;
        errno = 0;
        if (!(tgkill(root_pid, root_pid, SIGUSR2) < 0 &&
              errno == EPERM)) bad |= 4;
        errno = 0;
        v.sival_int = 1;
        if (!(sigqueue(root_pid, SIGUSR2, v) < 0 && errno == EPERM)) bad |= 8;
        /* kill(-1): everybody it may signal -- which is only itself,
         * excluded, so nobody: EPERM, there being processes it may not
         * touch. */
        errno = 0;
        if (!(kill(-1, SIGUSR2) < 0 && (errno == EPERM || errno == ESRCH)))
            bad |= 16;
        /* Its own signals still work. */
        if (kill(getpid(), 0) != 0) bad |= 32;
        _exit(bad);
    }
    waitpid(child, &st, 0);
    printf("  (the nobody's result: %d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    check("a nobody's kill, kill 0, tgkill and sigqueue to root's process: EPERM",
          WIFEXITED(st) && (WEXITSTATUS(st) & 15) == 0);
    check("  and kill(-1) reaches nobody it may not", WIFEXITED(st) &&
          (WEXITSTATUS(st) & 16) == 0);
    check("  while it may still signal itself", WIFEXITED(st) &&
          (WEXITSTATUS(st) & 32) == 0);
    check("  and root's process received none of it", usr2_count == 0);

    /* --- only a process itself may claim kill(2) sent it ------------ */
    {
        /* The raw call takes Linux's signal number, which is not this
         * library's: SIGUSR2 is 12 to the kernel. */
        enum { LINUX_SIGUSR2 = 12 };
        struct {
            int signo, err, code, pid, uid, val, pad[26];
        } k;
        long r;

        memset(&k, 0, sizeof(k));
        k.signo = LINUX_SIGUSR2;
        k.code = SI_USER;
        child = fork();
        if (child == 0) {
            pause();
            _exit(0);
        }
        errno = 0;
        r = syscall(SYS_rt_sigqueueinfo, child, LINUX_SIGUSR2, &k);
        check("rt_sigqueueinfo claiming SI_USER, to another process: EPERM",
              r < 0 && errno == EPERM);
        kill(child, SIGKILL);
        waitpid(child, &st, 0);
        usr2_count = 0;
        r = syscall(SYS_rt_sigqueueinfo, me, LINUX_SIGUSR2, &k);
        check("  and to itself, allowed", r == 0 && usr2_count == 1);
    }

    printf("sigqtest: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
