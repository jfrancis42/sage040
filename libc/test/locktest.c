/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * locktest - fcntl record locks: POSIX (per process) and OFD (per open
 * file description), and F_DUPFD's argument.
 *
 * A process's own locks never conflict with each other, so almost every
 * check here is made from a SECOND process, which is the only thing that
 * can see a lock at all. Each child reports through its exit status.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures, checks;
static const char *file = "/lock.dat";

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

static int lk(int fd, int cmd, int type, long start, long len)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = (short)type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    return fcntl(fd, cmd, &fl);
}

/* In a child: may this process take `type` over [start, start+len)? */
static int child_can(int type, long start, long len)
{
    int st;
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open(file, O_RDWR);

        _exit(lk(fd, F_SETLK, type, start, len) == 0 ? 0 :
              errno == EAGAIN ? 1 : 2);
    }
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

static void posix_locks(void)
{
    struct flock fl;
    int fd, fd2, st, p[2];
    pid_t pid;
    long t0;
    char c;

    fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "0123456789", 10);
    check("F_SETLK F_WRLCK on bytes 0-99", lk(fd, F_SETLK, F_WRLCK, 0, 100) == 0);
    check("  another process cannot take any of it: EAGAIN",
          child_can(F_WRLCK, 50, 10) == 1 && child_can(F_RDLCK, 99, 1) == 1);
    check("  but can take what is next to it", child_can(F_WRLCK, 100, 100) == 0);

    pid = fork();
    if (pid == 0) {
        int cfd = open(file, O_RDWR);

        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 50;
        fl.l_len = 10;
        _exit(fcntl(cfd, F_GETLK, &fl) == 0 && fl.l_type == F_WRLCK &&
              fl.l_start == 0 && fl.l_len == 100 && fl.l_pid == getppid()
              ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    check("  F_GETLK from another process names it: type, range, holder",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_start = 0;
    fl.l_len = 100;
    check("  and from the holder itself: F_UNLCK, nothing in its way",
          fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_UNLCK);

    check("unlocking the middle, 40-59, splits it",
          lk(fd, F_SETLK, F_UNLCK, 40, 20) == 0 &&
          child_can(F_WRLCK, 40, 20) == 0 && child_can(F_WRLCK, 39, 1) == 1 &&
          child_can(F_WRLCK, 60, 1) == 1);
    check("converting part to a read lock lets readers in there only",
          lk(fd, F_SETLK, F_RDLCK, 0, 20) == 0 &&
          child_can(F_RDLCK, 0, 20) == 0 && child_can(F_WRLCK, 10, 1) == 1 &&
          child_can(F_RDLCK, 20, 1) == 1);

    /* The POSIX wart: closing ANY descriptor for the file drops them all. */
    fd2 = open(file, O_RDONLY);
    close(fd2);
    check("closing another descriptor for the file drops every lock the "
          "process had on it", child_can(F_WRLCK, 0, 100) == 0);

    lk(fd, F_SETLK, F_WRLCK, 0, 0);
    pid = fork();
    if (pid == 0) {
        int cfd = open(file, O_RDWR);
        long start = now_ms();
        int r = lk(cfd, F_SETLKW, F_WRLCK, 5, 1);

        _exit(r == 0 && now_ms() - start >= 250 ? 0 : 1);
    }
    ms_sleep(300);
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    waitpid(pid, &st, 0);
    check("F_SETLKW waits for the holder to let go, then has it",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* EDEADLK: each holds one byte and wants the other's. */
    lk(fd, F_SETLK, F_WRLCK, 0, 1);
    pipe(p);
    pid = fork();
    if (pid == 0) {
        int cfd = open(file, O_RDWR);

        lk(cfd, F_SETLK, F_WRLCK, 1, 1);
        write(p[1], "x", 1);
        _exit(lk(cfd, F_SETLKW, F_WRLCK, 0, 1) == 0 ? 0 : 1);
    }
    read(p[0], &c, 1);
    ms_sleep(200);              /* the child is asleep on byte 0 by now */
    check("F_SETLKW that would close a circle of waiters is EDEADLK",
          lk(fd, F_SETLKW, F_WRLCK, 1, 1) == -1 && errno == EDEADLK);
    lk(fd, F_SETLK, F_UNLCK, 0, 1);
    waitpid(pid, &st, 0);
    check("  and the other waiter gets it when this one lets go",
          WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(p[0]);
    close(p[1]);

    pid = fork();
    if (pid == 0) {
        int cfd = open(file, O_RDWR);

        lk(cfd, F_SETLK, F_WRLCK, 0, 0);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    check("a process's locks go when it exits",
          lk(fd, F_SETLK, F_WRLCK, 0, 0) == 0);
    lk(fd, F_SETLK, F_UNLCK, 0, 0);

    fd2 = open(file, O_RDONLY);
    check("F_WRLCK on a descriptor open only for reading is EBADF",
          lk(fd2, F_SETLK, F_WRLCK, 0, 1) == -1 && errno == EBADF);
    close(fd2);                 /* (and drops nothing: there is nothing) */

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_END;
    fl.l_start = -4;
    fl.l_len = 4;
    check("SEEK_END: the last four bytes of a ten-byte file are 6-9",
          fcntl(fd, F_SETLK, &fl) == 0 && child_can(F_WRLCK, 6, 1) == 1 &&
          child_can(F_WRLCK, 5, 1) == 0 && child_can(F_WRLCK, 10, 1) == 0);
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    check("a negative length reaches back from the start: -3 at 5 is 2-4",
          lk(fd, F_SETLK, F_WRLCK, 5, -3) == 0 &&
          child_can(F_WRLCK, 2, 3) == 1 && child_can(F_WRLCK, 1, 1) == 0 &&
          child_can(F_WRLCK, 5, 1) == 0);
    lk(fd, F_SETLK, F_UNLCK, 0, 0);
    check("an l_type that is no lock at all is EINVAL",
          lk(fd, F_SETLK, 77, 0, 1) == -1 && errno == EINVAL);
    close(fd);
}

static void ofd_locks(void)
{
    struct flock fl;
    int a, b, d, x;

    a = open(file, O_RDWR);
    b = open(file, O_RDWR);
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_len = 10;
    check("F_OFD_SETLK on one open of the file",
          fcntl(a, F_OFD_SETLK, &fl) == 0);
    check("  conflicts with a second open() IN THE SAME PROCESS",
          fcntl(b, F_OFD_SETLK, &fl) == -1 && errno == EAGAIN);
    d = dup(a);
    check("  but not with a dup of the first, which is the same description",
          fcntl(d, F_OFD_SETLK, &fl) == 0);
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_len = 1;
    check("  F_OFD_GETLK names the holder as l_pid -1",
          fcntl(b, F_OFD_GETLK, &fl) == 0 && fl.l_type == F_WRLCK &&
          fl.l_pid == -1);
    x = open(file, O_RDONLY);
    close(x);
    fl.l_type = F_WRLCK;
    fl.l_pid = 0;               /* F_OFD_GETLK filled in -1 */
    check("  closing some other descriptor of the file does not drop it",
          fcntl(b, F_OFD_SETLK, &fl) == -1 && errno == EAGAIN);
    close(d);
    check("  nor does closing one of the description's two",
          fcntl(b, F_OFD_SETLK, &fl) == -1 && errno == EAGAIN);
    close(a);
    check("  closing the last does",
          fcntl(b, F_OFD_SETLK, &fl) == 0);
    fl.l_pid = 1;
    check("  and an OFD request with l_pid set is EINVAL",
          fcntl(b, F_OFD_SETLK, &fl) == -1 && errno == EINVAL);
    close(b);
}

static void dupfd(void)
{
    int n = fcntl(0, F_DUPFD, 20);
    int m = fcntl(0, F_DUPFD_CLOEXEC, 25);

    check("F_DUPFD takes its argument: the lowest free from 20", n == 20);
    check("F_DUPFD_CLOEXEC too, and sets FD_CLOEXEC",
          m == 25 && fcntl(m, F_GETFD) == FD_CLOEXEC);
    close(n);
    close(m);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        file = argv[1];
    }
    printf("  -- %s\n", file);
    posix_locks();
    ofd_locks();
    dupfd();
    unlink(file);
    printf("locktest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
