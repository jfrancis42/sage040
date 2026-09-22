/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pipetest - pipes, descriptors and process groups, and a handful of
 * small tools to build pipelines out of.
 *
 *   pipetest              the in-process checks
 *   pipetest out TEXT     TEXT and a newline, to stdout
 *   pipetest err TEXT     TEXT and a newline, to stderr
 *   pipetest cat          stdin to stdout
 *   pipetest count        count stdin's bytes and say how many
 *   pipetest gen N        N bytes of a known pattern, to stdout
 *   pipetest check N      read stdin and check it is gen N's output
 *   pipetest head        read one line of stdin, print it, and exit
 *   pipetest sleepy       wait for ever, reading nothing
 *   pipetest readtty      read one line from the terminal and print it
 */
#include "ulib.h"

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static u32 atou(const char *s)
{
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s++ - '0');
    }
    return v;
}

static u8 pattern(u32 i)
{
    return (u8)('a' + (i * 7) % 26);
}

static u8 buf[1500];

/* --- the tools ------------------------------------------------------- */

static int tool(int argc, char **argv)
{
    const char *m = argv[1];
    s32 n;

    if (strcmp(m, "out") == 0 && argc > 2) {
        puts(argv[2]);
        putch('\n');
        return 0;
    }
    if (strcmp(m, "err") == 0 && argc > 2) {
        eputs(argv[2]);
        eputs("\n");
        return 0;
    }
    if (strcmp(m, "cat") == 0) {
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            write(1, buf, (u32)n);
        }
        return n < 0 ? 1 : 0;
    }
    if (strcmp(m, "count") == 0) {
        u32 total = 0;

        while ((n = read(0, buf, sizeof(buf))) > 0) {
            total += (u32)n;
        }
        puts("pipetest: counted ");
        putdec(total);
        puts(" bytes\n");
        return 0;
    }
    if (strcmp(m, "gen") == 0 && argc > 2) {
        u32 want = atou(argv[2]), i = 0;

        while (i < want) {
            u32 k, chunk = want - i < sizeof(buf) ? want - i : sizeof(buf);

            for (k = 0; k < chunk; k++) {
                buf[k] = pattern(i + k);
            }
            if (write(1, buf, chunk) != (s32)chunk) {
                return 1;
            }
            i += chunk;
        }
        return 0;
    }
    if (strcmp(m, "check") == 0 && argc > 2) {
        u32 want = atou(argv[2]), i = 0;
        int ok = 1;

        while ((n = read(0, buf, sizeof(buf))) > 0) {
            s32 k;

            for (k = 0; k < n; k++) {
                if (buf[k] != pattern(i + (u32)k)) {
                    ok = 0;
                }
            }
            i += (u32)n;
        }
        puts(ok && i == want ? "pipetest: check passed, " :
                               "pipetest: check FAILED, ");
        putdec(i);
        puts(" bytes\n");
        return 0;
    }
    if (strcmp(m, "head") == 0) {
        char c;

        while (read(0, &c, 1) == 1) {
            putch(c);
            if (c == '\n') {
                break;
            }
        }
        return 0;
    }
    if (strcmp(m, "sleepy") == 0) {
        puts("pipetest: sleepy\n");
        for (;;) {
            pause();
        }
    }
    if (strcmp(m, "readtty") == 0) {
        static char line[80];

        n = read(0, line, sizeof(line) - 1);
        if (n > 0) {
            line[n] = '\0';
            puts("pipetest: read ");
            puts(line);
        } else {
            puts("pipetest: read failed\n");
        }
        return 0;
    }
    puts("pipetest: unknown mode\n");
    return 2;
}

/* --- the checks ------------------------------------------------------ */

static volatile int got_pipe;

static void on_sigpipe(int sig)
{
    got_pipe = sig;
}

static void test_pipes(void)
{
    int p[2], q[2], i, ok;
    struct stat st;
    struct pollfd pf;
    char c;
    s32 n;

    report("pipe() makes two descriptors", pipe(p) == 0 && p[0] != p[1]);
    report("  and neither is a terminal", !isatty(p[0]) && !isatty(p[1]));
    report("  and fstat calls them a FIFO",
           fstat(p[0], &st) == 0 && S_ISFIFO(st.st_mode));

    report("what goes in one end comes out the other",
           write(p[1], "hello", 5) == 5 && read(p[0], buf, 16) == 5 &&
           memcmp(buf, "hello", 5) == 0);

    /* Readiness. */
    pf.fd = p[0];
    pf.events = POLLIN;
    report("an empty pipe is not readable", poll(&pf, 1, 0) == 0);
    write(p[1], "x", 1);
    report("  and one with a byte in it is", poll(&pf, 1, 0) == 1);
    read(p[0], &c, 1);

    /* Full, and O_NONBLOCK. */
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    report("F_SETFL sets O_NONBLOCK", (fcntl(p[1], F_GETFL, 0) & O_NONBLOCK) != 0);
    for (i = 0; i < PIPE_SIZE; i++) {
        if (write(p[1], "y", 1) != 1) {
            break;
        }
    }
    report("a pipe holds PIPE_SIZE bytes", i == PIPE_SIZE);
    report("  and one more is EAGAIN when non-blocking",
           write(p[1], "z", 1) == -EAGAIN);
    pf.fd = p[1];
    pf.events = POLLOUT;
    report("  and a full pipe is not writable", poll(&pf, 1, 0) == 0);
    fcntl(p[1], F_SETFL, 0);

    /* End of file. */
    close(p[1]);
    n = 0;
    ok = 1;
    for (;;) {
        s32 r = read(p[0], buf, sizeof(buf));

        if (r <= 0) {
            ok = r == 0;
            break;
        }
        n += r;
    }
    report("with the writer gone, the reader drains it and then gets EOF",
           ok && n == PIPE_SIZE);
    pf.fd = p[0];
    pf.events = POLLIN;
    report("  and poll says it has hung up",
           poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP));
    close(p[0]);

    /* The reader gone: SIGPIPE, then EPIPE. */
    pipe(q);
    close(q[0]);
    signal(SIGPIPE, on_sigpipe);
    report("writing to a pipe nobody can read is EPIPE",
           write(q[1], "x", 1) == -EPIPE);
    report("  after raising SIGPIPE", got_pipe == SIGPIPE);
    close(q[1]);
    signal(SIGPIPE, SIG_DFL);

    /* Descriptors. */
    pipe(p);
    report("a new descriptor is not close-on-exec",
           fcntl(p[0], F_GETFD, 0) == 0);
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    report("F_SETFD sets FD_CLOEXEC", fcntl(p[0], F_GETFD, 0) == FD_CLOEXEC);
    n = fcntl(p[0], F_DUPFD, 20);
    report("F_DUPFD gives the lowest free descriptor from its argument",
           n == 20);
    report("  and the duplicate is not close-on-exec",
           fcntl((int)n, F_GETFD, 0) == 0);
    close((int)n);
    report("dup2 onto a descriptor clears close-on-exec",
           dup2(p[0], 21) == 21 && fcntl(21, F_GETFD, 0) == 0);
    close(21);
    close(p[0]);
    close(p[1]);

    report("the system allows 32 descriptors",
           fcntl(0, F_DUPFD, 31) == 31 && fcntl(0, F_DUPFD, 32) == -EINVAL);
    close(31);

    /* Process groups. */
    report("a program's parent is the shell", getppid() > 0);
    report("  and the shell put it in a group of its own",
           getpgrp() == getpid());
    report("getpgid(0) agrees", syscall(__NR_getpgid, 0) == getpid());
    report("the terminal's foreground group is this one",
           ioctl(0, TIOCGPGRP, (u32)&i) == 0 && i == getpid());
    report("setpgid into a group that does not exist is EPERM",
           syscall(__NR_setpgid, 0, 9999) == -EPERM);
}

/* A child in this group ending with kill(0, ...) -- the group form. */
static void test_group_kill(void)
{
    static char *argv[3] = { "/PIPETEST", "sleepy", 0 };
    int child, st = 0;

    child = spawn("/PIPETEST", 2, argv, 0);
    report("a spawned helper joins its parent's group",
           syscall(__NR_getpgid, child) == getpgrp());
    signal(SIGTERM, SIG_IGN);           /* spare this one */
    report("kill(0, SIGTERM) reaches the whole group",
           kill(0, SIGTERM) == 0 && waitpid(child, &st, 0) == child &&
           st == 128 + SIGTERM);
    signal(SIGTERM, SIG_DFL);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        return tool(argc, argv);
    }
    test_pipes();
    test_group_kill();
    puts("pipetest: done\n");
    return 0;
}
