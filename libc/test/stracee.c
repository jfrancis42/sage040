/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * stracee - a program that makes a known list of system calls, for
 * kernel/stracetest.sh to watch strace describe. Each has an argument
 * or a result strace could only print by reading the program's memory
 * and registers correctly.
 *
 *     stracee            the calls, then exit 7
 *     stracee wait       loop until killed, for strace -p
 */
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int st = 0;
    pid_t p;

    if (argc > 1 && strcmp(argv[1], "wait") == 0) {
        for (;;) {
            write(1, "tick\n", 5);
            sleep(1);
        }
    }
    write(1, "STRACEE says hello\n", 19);
    getpid();
    open("/no/such/file", O_RDONLY);
    p = fork();
    if (p == 0) {
        write(1, "child\n", 6);
        _exit(3);
    }
    waitpid(p, &st, 0);
    kill(getpid(), 0);
    return 7;
}
