/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ptytest.c - pseudo-terminals: a terminal with a program at each end.
 *
 * WHAT HAS TO BE TRUE for a pty to be worth having, and what each check
 * below is for:
 *
 *   - the slave must BE a terminal: isatty(), termios, a window size.
 *     A pipe would pass everything else here and fail that, and a
 *     program decides how to behave by asking exactly that question;
 *   - what the master writes must arrive at the slave as INPUT, through
 *     the line discipline -- so a canonical read gets one line at a
 *     time, and the erase character erases;
 *   - what the slave writes must come back out of the master, with the
 *     output processing a terminal does (a newline becomes CR LF);
 *   - echo must go to the MASTER, because that is where the person is;
 *   - ctrl-C on the master must raise SIGINT in the process group on
 *     the slave, which is the whole difference between a terminal and a
 *     pipe;
 *   - and closing one end must be visible at the other, or a program
 *     whose terminal has gone waits for ever.
 */
#define _GNU_SOURCE             /* posix_openpt and the rest: XSI */

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;

static void
check(const char *what, int ok)
{
    if (ok) {
        pass++;
        printf("  [ OK ] %s\n", what);
    } else {
        fail++;
        printf("  [FAIL] %s\n", what);
    }
    fflush(stdout);
}

/* Read what is there, up to `len`, waiting a little for it to arrive:
 * the other end is a program that has to be scheduled. */
static int
slurp(int fd, char *buf, int len, int tries)
{
    int n = 0, i;

    for (i = 0; i < tries && n < len - 1; i++) {
        int r;
        struct timespec ms = { 0, 50000000L };

        r = (int)read(fd, buf + n, (size_t)(len - 1 - n));
        if (r > 0) {
            n += r;
            i = 0;              /* more may be coming */
            continue;
        }
        nanosleep(&ms, 0);
    }
    buf[n] = '\0';
    return n;
}

static volatile sig_atomic_t got_int;

static void
on_int(int sig)
{
    (void)sig;
    got_int = 1;
}

int
main(void)
{
    int master = -1, slave = -1;
    char name[64];
    char buf[512];

    printf("ptytest: pseudo-terminals on SuckOS\n");

    printf("=== opening a pair ===\n");
    master = posix_openpt(O_RDWR | O_NOCTTY);
    check("posix_openpt gives a master", master >= 0);
    check("grantpt", grantpt(master) == 0);
    check("unlockpt", unlockpt(master) == 0);
    check("ptsname names the other end (%s)",
          ptsname_r(master, name, sizeof(name)) == 0 &&
          strncmp(name, "/dev/pts/", 9) == 0);
    printf("         the slave is %s\n", name);

    slave = open(name, O_RDWR);
    check("the slave opens by that name", slave >= 0);

    /*
     * BOTH ENDS NON-BLOCKING, and they have to be: this program is on
     * both sides of the terminal, so a blocking read of an end nobody
     * is going to write to again is a program waiting for itself. A
     * real user of a pty has something else at the other end.
     */
    if (slave >= 0 && master >= 0) {
        fcntl(master, F_SETFL, O_NONBLOCK);
        fcntl(slave, F_SETFL, O_NONBLOCK);
    }
    if (slave < 0 || master < 0) {
        printf("\n  passed: %d\n  failed: %d\nRESULT: FAIL\n", pass, fail + 1);
        return 1;
    }

    printf("=== the slave is a terminal ===\n");
    check("isatty(slave)", isatty(slave) == 1);
    check("  and isatty(master) too", isatty(master) == 1);
    {
        struct termios t;
        struct winsize w;

        check("tcgetattr on the slave", tcgetattr(slave, &t) == 0);
        check("  it starts canonical, echoing, with signals",
              (t.c_lflag & ICANON) && (t.c_lflag & ECHO) && (t.c_lflag & ISIG));
        check("  its default size is 80x24",
              ioctl(slave, TIOCGWINSZ, &w) == 0 &&
              w.ws_col == 80 && w.ws_row == 24);
        w.ws_col = 132;
        w.ws_row = 43;
        check("  and the size can be set from the master",
              ioctl(master, TIOCSWINSZ, &w) == 0 &&
              ioctl(slave, TIOCGWINSZ, &w) == 0 &&
              w.ws_col == 132 && w.ws_row == 43);
    }

    printf("=== typing at it ===\n");
    write(master, "hello\n", 6);
    {
        int n = slurp(slave, buf, sizeof(buf), 20);

        check("what the master wrote arrives at the slave",
              n == 6 && strcmp(buf, "hello\n") == 0);
    }
    {
        /* Echo went the other way, to where the person is. */
        int n = slurp(master, buf, sizeof(buf), 10);

        check("  and was echoed back to the master, with CR LF",
              n >= 6 && strstr(buf, "hello\r\n") != 0);
    }

    printf("=== the line discipline ===\n");
    /*
     * Type "ab", erase the b, type "c": the program on the terminal
     * must see "ac" and never the b, because erasing is the
     * discipline's job and not its.
     */
    write(master, "ab\177c\n", 5);     /* DEL is the erase character */
    slurp(master, buf, sizeof(buf), 6); /* drop the echo */
    {
        int n = slurp(slave, buf, sizeof(buf), 20);

        check("the erase character erased one character",
              n == 3 && strcmp(buf, "ac\n") == 0);
    }
    {
        /* A canonical read returns ONE line, whatever else is waiting. */
        int n;

        write(master, "one\ntwo\n", 8);
        slurp(master, buf, sizeof(buf), 6);
        n = (int)read(slave, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = '\0';
        check("a canonical read returns one line at a time",
              n == 4 && strcmp(buf, "one\n") == 0);
        n = (int)read(slave, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = '\0';
        check("  and the next read returns the next", n == 4 &&
                                                      strcmp(buf, "two\n") == 0);
    }
    {
        /* Raw mode: every character as it arrives, no echo. */
        struct termios t;

        tcgetattr(slave, &t);
        t.c_lflag &= ~(unsigned)(ICANON | ECHO);
        check("the slave can be put in raw mode",
              tcsetattr(slave, TCSANOW, &t) == 0);
        write(master, "xy", 2);
        {
            int n = slurp(slave, buf, sizeof(buf), 20);

            check("  a raw read gets characters without a newline",
                  n == 2 && strcmp(buf, "xy") == 0);
        }
        {
            int n = (int)read(master, buf, sizeof(buf) - 1);

            check("  and nothing was echoed", n <= 0);
        }
        t.c_lflag |= ICANON | ECHO;
        tcsetattr(slave, TCSANOW, &t);
    }

    printf("=== output from the program on it ===\n");
    write(slave, "line\n", 5);
    {
        int n = slurp(master, buf, sizeof(buf), 20);

        check("what the slave writes comes out of the master as CR LF",
              n == 6 && strcmp(buf, "line\r\n") == 0);
    }

    printf("=== ctrl-C reaches the program, not the parent ===\n");
    {
        struct sigaction sa;
        pid_t child;
        int status = 0;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_int;
        sigaction(SIGINT, &sa, 0);


        child = fork();
        if (child == 0) {
            /*
             * THE HANDLER IS INHERITED, and must go: a child that
             * kept its parent's SIGINT handler would catch the ctrl-C
             * and carry on, and this test would be waiting for a death
             * that was never going to happen. A program on a terminal
             * normally has the default disposition, which is to die.
             */
            struct sigaction dfl;

            memset(&dfl, 0, sizeof(dfl));
            dfl.sa_handler = SIG_DFL;
            sigaction(SIGINT, &dfl, 0);
            /* The child's whole world is the pty: its own session, the
             * slave as its terminal, and it is the foreground group. */
            int pg;

            close(master);
            setsid();
            pg = getpid();
            ioctl(slave, TIOCSPGRP, &pg);
            setpgid(0, 0);
            for (;;) {
                pause();        /* until SIGINT ends it */
            }
        }
        {
            int pg = child;
            struct timespec ms = { 0, 200000000L };

            nanosleep(&ms, 0);
            ioctl(master, TIOCSPGRP, &pg);
            write(master, "\003", 1);       /* ctrl-C */
            check("the child died of a signal",
                  waitpid(child, &status, 0) == child && WIFSIGNALED(status));
            check("  and the signal was SIGINT", WTERMSIG(status) == SIGINT);
            check("  while the parent, not in that group, was untouched",
                  got_int == 0);
        }
    }

    printf("=== closing an end ===\n");
    {
        int m2, s2;
        char n2[64];

        check("openpty gives both ends at once",
              openpty(&m2, &s2, n2, 0, 0) == 0);
        fcntl(m2, F_SETFL, O_NONBLOCK);
        close(s2);
        {
            int n = (int)read(m2, buf, sizeof(buf));

            check("  with the slave closed, the master reads end of file",
                  n == 0);
        }
        close(m2);

        check("another pair, to close the other way",
              openpty(&m2, &s2, n2, 0, 0) == 0);
        fcntl(s2, F_SETFL, O_NONBLOCK);
        close(m2);
        {
            int n = (int)read(s2, buf, sizeof(buf));

            check("  with the master closed, the slave reads end of file",
                  n == 0);
        }
        close(s2);
    }

    printf("=== the pairs are given back ===\n");
    {
        /*
         * There are a fixed number of pairs. Take every one, give them
         * all back, and take them all again: a pair that leaked would
         * make the second round come up short, and nothing else here
         * would notice -- a leak looks exactly like success until the
         * table is empty.
         */
        int fds[32], n = 0, again = 0, i;

        while (n < (int)(sizeof(fds) / sizeof(fds[0]))) {
            int fd = posix_openpt(O_RDWR | O_NOCTTY);

            if (fd < 0) {
                break;
            }
            fds[n++] = fd;
        }
        check("every pair can be taken at once", n > 0);
        printf("         %d pairs\n", n);
        for (i = 0; i < n; i++) {
            close(fds[i]);
        }
        while (again < n) {
            int fd = posix_openpt(O_RDWR | O_NOCTTY);

            if (fd < 0) {
                break;
            }
            fds[again++] = fd;
        }
        check("  and taken again after being closed -- none leaked",
              again == n);
        for (i = 0; i < again; i++) {
            close(fds[i]);
        }
    }

    close(slave);
    close(master);

    printf("\n  passed: %d\n  failed: %d\n", pass, fail);
    printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
    printf("PTY-SECOND\n");    /* the harness waits for this */
    return fail ? 1 : 0;
}
