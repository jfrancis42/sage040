/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * limits - the static limits, reached and passed.
 *
 * Each one is filled to the number the ABI says (OPEN_MAX, and the
 * machine's own task table), and then one more is asked for: it must be
 * refused with the errno Linux gives, and nothing else may go wrong. A
 * limit nobody has reached is a limit nobody knows is right.
 */
#include "ulib.h"

static int failures;
static const char *self = "/limits";

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
    if (!ok) {
        failures++;
    }
}

static void say(const char *what, u32 v)
{
    puts("limits: ");
    puts(what);
    putch(' ');
    putdec(v);
    putch('\n');
}

/* Descriptors: every one of OPEN_MAX, on one file opened again and
 * again -- each open is a handle of the filesystem's own, so this also
 * reaches the machine-wide FAT table's first 61. */
static void test_files(void)
{
    int fds[OPEN_MAX + 1];
    int n = 3, i, ok = 1, last;
    char c;

    while (n < OPEN_MAX) {
        fds[n] = open(self, O_RDONLY);
        if (fds[n] < 0) {
            break;
        }
        n++;
    }
    say("descriptors open:", (u32)n);
    report("a task can have OPEN_MAX descriptors (64)", n == OPEN_MAX);
    last = open(self, O_RDONLY);
    report("  and one more is EMFILE", last == -EMFILE);
    for (i = 3; i < n; i++) {
        if (read(fds[i], &c, 1) != 1) {
            ok = 0;
        }
    }
    report("  and every one of them reads", ok);
    for (i = 3; i < n; i++) {
        close(fds[i]);
    }
    report("  and closing them gives them back",
           (i = open(self, O_RDONLY)) >= 0 && close(i) == 0);
}

/* The FAT's machine-wide table: children, each holding files, past what
 * any one task may. */
static void test_fs_handles(void)
{
    int p[2], k, pid[3], st, total = 0;

    pipe(p);
    for (k = 0; k < 3; k++) {
        pid[k] = fork();
        if (pid[k] == 0) {
            int n = 0;
            char go;

            close(p[0]);
            while (n < 40 && open(self, O_RDONLY) >= 0) {
                n++;
            }
            write(p[1], &n, sizeof(n));
            msleep(1500);           /* hold them while the others count */
            (void)go;
            exit(0);
        }
    }
    close(p[1]);
    for (k = 0; k < 3; k++) {
        int n = 0;

        read(p[0], &n, sizeof(n));
        total += n;
    }
    for (k = 0; k < 3; k++) {
        waitpid(pid[k], &st, 0);
    }
    close(p[0]);
    say("files open at once across three tasks:", (u32)total);
    report("the machine holds 120 files open at once, across tasks",
           total == 120);
}

/* Tasks: fork until refused, each child waiting to be told to go. */
static void test_tasks(void)
{
    int p[2], pids[80], n = 0, i, st, refused = 0;

    pipe(p);
    for (;;) {
        int pid = fork();

        if (pid == 0) {
            char c;

            close(p[1]);
            read(p[0], &c, 1);      /* until the parent closes the pipe */
            exit(0);
        }
        if (pid < 0) {
            refused = pid;
            break;
        }
        pids[n++] = pid;
        if (n == 80) {
            break;
        }
    }
    say("children forked:", (u32)n);
    report("fork fills the task table and is then refused with EAGAIN",
           n >= 50 && refused == -EAGAIN);
    close(p[1]);                    /* every child's read ends */
    for (i = 0; i < n; i++) {
        waitpid(pids[i], &st, 0);
    }
    close(p[0]);
    {
        int pid = fork();

        if (pid == 0) {
            exit(7);
        }
        report("  and once they have gone, fork works again",
               pid > 0 && waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 7);
    }
}

/* Sockets: pairs until refused. */
static void test_sockets(void)
{
    int sv[64][2], n = 0, i;

    while (n < 64 && socketpair(AF_UNIX, SOCK_STREAM, 0, sv[n]) == 0) {
        n++;
        if (2 * n + 3 >= OPEN_MAX - 1) {
            break;                  /* descriptors, not sockets, first */
        }
    }
    say("socket pairs:", (u32)n);
    report("30 socket pairs in one task", n >= 30);
    for (i = 0; i < n; i++) {
        close(sv[i][0]);
        close(sv[i][1]);
    }
}

int main(int argc, char **argv)
{
    if (argc > 0 && argv[0] && argv[0][0]) {
        self = argv[0];
    }
    (void)argc;
    test_files();
    test_fs_handles();
    test_tasks();
    test_sockets();
    puts("limits: ");
    putdec((u32)failures);
    puts(" failed\n");
    return failures != 0;
}
