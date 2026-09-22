/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * polltest - poll() and select().
 *
 * Plain `polltest` checks what can be checked without anybody's help:
 * timeouts that are kept, files that are always ready, descriptors that
 * are not open, the limits, and a signal cutting a wait short.
 *
 *   polltest tty           wait for a key with poll(), then with
 *                          select(); the harness types them
 *   polltest net IP PORT   fetch /small.txt from a web server, waiting
 *                          with poll() for every step
 */
#include "ulib.h"

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static u32 now_ms(void)
{
    return times(0) * (1000 / HZ);
}

static volatile int got;

static void on_usr1(int sig)
{
    got = sig;
}

static void utoa(char *out, u32 v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *out++ = tmp[--n];
    }
    *out = '\0';
}

static void test_basics(void)
{
    struct pollfd p[3];
    struct timeval tv;
    fd_set in, out;
    u32 t0, took;
    int fd, r;

    /* No descriptors at all is a sleep. */
    t0 = now_ms();
    r = poll(0, 0, 300);
    took = now_ms() - t0;
    report("poll with nothing to watch waits out its timeout",
           r == 0 && took >= 290 && took < 600);

    fd = open("/POLLTEST", O_RDONLY);
    p[0].fd = fd;
    p[0].events = POLLIN | POLLOUT;
    p[1].fd = -1;               /* ignored */
    p[1].events = POLLIN;
    p[2].fd = 7;                /* not open */
    p[2].events = POLLIN;
    t0 = now_ms();
    r = poll(p, 3, 5000);
    took = now_ms() - t0;
    report("a regular file is ready at once", r == 2 && took < 100);
    report("  both ways", p[0].revents == (POLLIN | POLLOUT));
    report("a negative descriptor is ignored", p[1].revents == 0);
    report("a closed one is POLLNVAL", p[2].revents == POLLNVAL);

    report("poll of too many descriptors is EINVAL",
           poll(p, 1000, 0) == -EINVAL);

    FD_ZERO(&in);
    FD_SET(fd, &in);
    FD_ZERO(&out);
    FD_SET(fd, &out);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    r = select(fd + 1, &in, &out, 0, &tv);
    report("select counts a file ready both ways as two",
           r == 2 && FD_ISSET(fd, &in) && FD_ISSET(fd, &out));
    report("  and writes back the time it did not use",
           tv.tv_sec == 0 ? tv.tv_usec > 900000 : tv.tv_sec == 1);

    FD_ZERO(&in);
    FD_SET(6, &in);
    report("select on a descriptor that is not open is EBADF",
           select(7, &in, 0, 0, 0) == -EBADF);

    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    t0 = now_ms();
    r = select(0, 0, 0, 0, &tv);
    took = now_ms() - t0;
    report("select with no sets is a 250 ms sleep",
           r == 0 && took >= 240 && took < 500);
    report("  and leaves no time over",
           tv.tv_sec == 0 && tv.tv_usec == 0);
    close(fd);

    /* A signal cuts the wait short: EINTR after a handler. */
    {
        static char pid[12];
        char *argv[5];
        int child, st;

        signal(SIGUSR1, on_usr1);
        utoa(pid, (u32)getpid());
        argv[0] = "/SIGTEST";
        argv[1] = "poke";
        argv[2] = pid;
        argv[3] = "10";
        argv[4] = "200";
        child = spawn("/SIGTEST", 5, argv, 0);
        t0 = now_ms();
        r = poll(0, 0, 3000);
        took = now_ms() - t0;
        report("a signal ends a poll with EINTR, not its timeout",
               r == -EINTR && got == SIGUSR1 && took < 1500);
        waitpid(child, &st, 0);
    }
}

static int test_tty(void)
{
    struct pollfd p;
    fd_set in;
    struct timeval tv;
    char c;
    u32 t0;
    int r;

    p.fd = 0;
    p.events = POLLIN;
    report("with nothing typed, stdin is not readable", poll(&p, 1, 0) == 0);

    puts("polltest: press a key for poll\n");
    t0 = now_ms();
    r = poll(&p, 1, 10000);
    report("poll woke for a key", r == 1 && (p.revents & POLLIN));
    report("  well before its timeout", now_ms() - t0 < 5000);
    if (read(0, &c, 1) == 1) {
        report("  and the key was there to read", c == 'k');
    }
    /* The rest of the line, so the next wait starts clean. */
    while (poll(&p, 1, 100) == 1 && read(0, &c, 1) == 1 && c != '\n') {
    }

    puts("polltest: press a key for select\n");
    FD_ZERO(&in);
    FD_SET(0, &in);
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    r = select(1, &in, 0, 0, &tv);
    report("select woke for a key", r == 1 && FD_ISSET(0, &in));
    report("  and left most of its ten seconds", tv.tv_sec >= 5);
    while (poll(&p, 1, 100) == 1 && read(0, &c, 1) == 1 && c != '\n') {
    }
    puts("polltest: tty done\n");
    return 0;
}

static int prefix(const char *s, const char *p)
{
    while (*p) {
        if (*s++ != *p++) {
            return 0;
        }
    }
    return 1;
}

static int test_net(const char *ip, const char *portstr)
{
    struct sockaddr_in a;
    struct pollfd p;
    static char buf[512];
    static const char req[] = "GET /small.txt HTTP/1.0\r\n\r\n";
    int fd, r, total = 0, found = 0;
    u32 port = 0;
    const char *q;

    for (q = portstr; *q; q++) {
        port = port * 10 + (u32)(*q - '0');
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr = inet_aton(ip);
    report("connected to the web server", connect(fd, &a) == 0);

    p.fd = fd;
    p.events = POLLOUT;
    report("a connected socket is writable", poll(&p, 1, 2000) == 1 &&
           (p.revents & POLLOUT));
    write(fd, req, sizeof(req) - 1);

    p.events = POLLIN;
    r = poll(&p, 1, 5000);
    report("poll woke when the reply arrived", r == 1 && (p.revents & POLLIN));

    for (;;) {
        s32 n;

        if (poll(&p, 1, 5000) != 1) {
            break;
        }
        n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            report("the end of the stream was reported readable", n == 0);
            break;
        }
        buf[n] = '\0';
        total += (int)n;
        {
            const char *s;

            for (s = buf; *s; s++) {
                if (prefix(s, "SMALL-FILE-OK")) {
                    found = 1;
                }
            }
        }
    }
    report("the whole reply was read through poll", total > 0 && found);
    close(fd);
    puts("polltest: net done\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "tty") == 0) {
        return test_tty();
    }
    if (argc > 3 && strcmp(argv[1], "net") == 0) {
        return test_net(argv[2], argv[3]);
    }
    test_basics();
    puts("polltest: done\n");
    return 0;
}
