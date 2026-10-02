/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * timeout - run a command, and stop it if it runs too long.
 *
 *   timeout [-s SIGNAL] [-k DURATION] [--preserve-status] DURATION CMD...
 *
 * As coreutils' timeout: DURATION is a number with an optional s, m, h
 * or d (and may have a fraction); 0 means no limit. When it expires the
 * command is sent SIGNAL (TERM unless -s says otherwise), and, with -k,
 * KILL that much later if it is still there. The exit status is the
 * command's, or 124 if it timed out (with --preserve-status, the
 * command's even then); 125 if timeout itself failed, 126 if the
 * command could not be run, 127 if it was not found.
 *
 * The command runs in a process group of its own when timeout is not
 * itself the foreground, so that the signal reaches everything it
 * started; coreutils does the same unless --foreground.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t expired;
static pid_t child;

static void on_alarm(int sig)
{
    (void)sig;
    expired = 1;
}

/* "1.5", "10s", "2m", "1h", "1d" in microseconds; -1 if not a duration. */
static long long parse_duration(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    double mult = 1;

    if (end == s || v < 0) {
        return -1;
    }
    switch (*end) {
    case '\0': case 's': break;
    case 'm': mult = 60; break;
    case 'h': mult = 3600; break;
    case 'd': mult = 86400; break;
    default: return -1;
    }
    if (*end && end[1]) {
        return -1;
    }
    return (long long)(v * mult * 1000000.0);
}

static int parse_signal(const char *s)
{
    static const struct { const char *name; int sig; } sigs[] = {
        { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT },
        { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
        { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CONT", SIGCONT },
        { "STOP", SIGSTOP },
    };
    unsigned i;
    char *end;
    long n = strtol(s, &end, 10);

    if (*s && !*end) {
        return n > 0 && n < NSIG ? (int)n : -1;
    }
    if (strncmp(s, "SIG", 3) == 0) {
        s += 3;
    }
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (strcmp(s, sigs[i].name) == 0) {
            return sigs[i].sig;
        }
    }
    return -1;
}

static void arm(long long us)
{
    struct itimerval it;

    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = (time_t)(us / 1000000);
    it.it_value.tv_usec = (suseconds_t)(us % 1000000);
    setitimer(ITIMER_REAL, &it, 0);
}

static void usage(void)
{
    fputs("usage: timeout [-s SIGNAL] [-k DURATION] [--preserve-status] "
          "DURATION COMMAND [ARG]...\n", stderr);
    exit(125);
}

int main(int argc, char **argv)
{
    int sig = SIGTERM, preserve = 0, i = 1, st = 0, timed_out = 0;
    long long limit, kill_after = 0;
    struct sigaction sa;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            if ((sig = parse_signal(argv[++i])) < 0) {
                fprintf(stderr, "timeout: %s: invalid signal\n", argv[i]);
                return 125;
            }
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            if ((kill_after = parse_duration(argv[++i])) < 0) {
                usage();
            }
        } else if (strcmp(argv[i], "--preserve-status") == 0) {
            preserve = 1;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            /* accepted: the command is never put in a group of its own
             * here when timeout is in the foreground anyway */
        } else {
            usage();
        }
    }
    if (argc - i < 2) {
        usage();            /* a duration and a command, at least */
    }
    if ((limit = parse_duration(argv[i])) < 0) {
        fprintf(stderr, "timeout: invalid time interval '%s'\n", argv[i]);
        return 125;
    }
    i++;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, 0);

    child = fork();
    if (child < 0) {
        perror("timeout: fork");
        return 125;
    }
    if (child == 0) {
        signal(SIGALRM, SIG_DFL);
        if (getpgrp() != tcgetpgrp(2)) {
            setpgid(0, 0);
        }
        execvp(argv[i], argv + i);
        fprintf(stderr, "timeout: failed to run command '%s': %s\n",
                argv[i], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    if (limit > 0) {
        arm(limit);
    }
    for (;;) {
        pid_t r = waitpid(child, &st, 0);

        if (r == child) {
            break;
        }
        if (r < 0 && errno == EINTR && expired) {
            expired = 0;
            if (!timed_out) {
                timed_out = 1;
                kill(child, sig);
                if (getpgid(child) == child) {
                    kill(-child, sig);
                }
                if (kill_after > 0) {
                    arm(kill_after);
                }
            } else {
                kill(child, SIGKILL);
                if (getpgid(child) == child) {
                    kill(-child, SIGKILL);
                }
            }
        }
    }
    if (timed_out && !preserve) {
        return 124;
    }
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    return 128 + WTERMSIG(st);
}
