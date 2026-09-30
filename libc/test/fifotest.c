/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fifotest - named pipes, in the directory given (the disk, or /tmp).
 *
 * Two processes that share nothing but a path talk through it; opening
 * one end waits for the other, and that wait is timed against
 * CLOCK_MONOTONIC in the process that did it. kernel/fifotest.sh leaves
 * a FIFO on the disk and has the host's e2fsck and debugfs look at it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

static void on_alarm(int sig)
{
    (void)sig;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "";
    char path[128], buf[64];
    struct stat st, st2;
    struct pollfd p;
    struct sigaction sa;
    int fd, fd2, n, stc;
    pid_t pid;

    snprintf(path, sizeof(path), "%s/fifo1", dir);
    printf("  -- %s\n", path);
    umask(022);
    unlink(path);
    check("mkfifo", mkfifo(path, 0666) == 0);
    check("  stat says FIFO, with the mode less the umask",
          stat(path, &st) == 0 && S_ISFIFO(st.st_mode) &&
          (st.st_mode & 0777) == 0644);
    check("  a second mkfifo of the name is EEXIST",
          mkfifo(path, 0666) == -1 && errno == EEXIST);

    /* The writer opens first and must wait for the reader. */
    pid = fork();
    if (pid == 0) {
        long t0 = now_ms();
        int w = open(path, O_WRONLY);
        char msg[32];

        snprintf(msg, sizeof(msg), "hello %ld", now_ms() - t0);
        write(w, msg, strlen(msg));
        close(w);
        _exit(w >= 0 ? 0 : 1);
    }
    ms_sleep(300);
    fd = open(path, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    n = (int)read(fd, buf, sizeof(buf) - 1);
    waitpid(pid, &stc, 0);
    check("another process's message arrives through the name",
          n > 6 && strncmp(buf, "hello ", 6) == 0);
    {
        long ms = 0;

        sscanf(buf + 6, "%ld", &ms);
        check("  and its open waited for this reader (250 ms or more)",
              ms >= 250);
        if (ms < 250) {
            printf("         (%ld ms)\n", ms);
        }
    }
    check("  and after the writer has gone, end of file",
          read(fd, buf, sizeof(buf)) == 0);
    fstat(fd, &st2);
    check("  fstat of the open FIFO: a FIFO, and the same inode as the name",
          S_ISFIFO(st2.st_mode) && st2.st_ino == st.st_ino);
    close(fd);

    fd = open(path, O_RDONLY | O_NONBLOCK);
    check("O_RDONLY|O_NONBLOCK with no writer opens at once, reading end "
          "of file", fd >= 0 && read(fd, buf, 1) == 0);
    fd2 = open(path, O_WRONLY | O_NONBLOCK);
    check("  and now a non-blocking writer can open: there is a reader",
          fd2 >= 0);
    write(fd2, "xy", 2);
    p.fd = fd;
    p.events = POLLIN;
    check("  poll sees what it wrote", poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    check("  and the reader reads it", read(fd, buf, sizeof(buf)) == 2 &&
          memcmp(buf, "xy", 2) == 0);
    close(fd2);
    close(fd);
    check("O_WRONLY|O_NONBLOCK with no reader is ENXIO",
          open(path, O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO);

    fd = open(path, O_RDWR);
    check("O_RDWR never waits, and is both ends",
          fd >= 0 && write(fd, "rw", 2) == 2 && read(fd, buf, 2) == 2 &&
          memcmp(buf, "rw", 2) == 0);
    close(fd);

    /* A blocked open is interrupted by a signal (no SA_RESTART). */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, 0);
    alarm(1);
    fd = open(path, O_RDONLY);
    check("a blocked open is ended by a signal: EINTR",
          fd == -1 && errno == EINTR);
    alarm(0);

    /* Unlinked while open: the pipe goes on; the name is gone. */
    fd = open(path, O_RDWR);
    unlink(path);
    check("unlinked while open: the pipe still works, the name is gone",
          write(fd, "u", 1) == 1 && read(fd, buf, 1) == 1 &&
          stat(path, &st) == -1 && errno == ENOENT);
    close(fd);

    snprintf(path, sizeof(path), "%s/cdev", dir);
    check("mknod of a character device is EPERM",
          mknod(path, S_IFCHR | 0600, 0x0101) == -1 && errno == EPERM);
    snprintf(path, sizeof(path), "%s/plain", dir);
    check("mknod S_IFREG makes an empty file",
          mknod(path, S_IFREG | 0600, 0) == 0 && stat(path, &st) == 0 &&
          S_ISREG(st.st_mode) && st.st_size == 0);
    unlink(path);

    /* One to leave for the host to look at, on the disk. */
    if (!*dir) {
        mkfifo("/keepfifo", 0640);
    }
    printf("fifotest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
