/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sesstest - sessions and controlling terminals (kernel/ctty.c), run by
 * kernel/sesstest.sh.
 *
 * Every check runs in children built for it -- a session leader, its
 * jobs, a second session -- because the rules are about who is in which
 * session, and the test process itself must not end up holding a
 * terminal. Results come back through pipes, so nothing depends on a
 * child's output reaching the console.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef TIOCGSID
#define TIOCGSID 0x5429
#endif

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

/* A new pty: the master, and its slave's name. */
static int new_pty(char *name, size_t size)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);

    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0 ||
        ptsname_r(m, name, size) != 0) {
        return -1;
    }
    return m;
}

/* Run fn in a child that has started its own session; its exit status
 * is the result (0 good). */
static int in_session(int (*fn)(void *), void *arg)
{
    int st = 0;
    pid_t p = fork();

    if (p == 0) {
        if (setsid() < 0) {
            _exit(90);
        }
        _exit(fn(arg));
    }
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

/* --- 1. a fresh session has no terminal ------------------------------ */
static int t_no_tty(void *unused)
{
    (void)unused;
    if (getsid(0) != getpid()) {
        return 1;
    }
    errno = 0;
    return (open("/dev/tty", O_RDWR) < 0 && errno == ENXIO) ? 0 : 2;
}

/* --- 2. a leader opening a pty gets it ------------------------------- */
struct pt { int master; char name[32]; };

static int t_acquire(void *v)
{
    struct pt *p = v;
    char buf[64], stat[512];
    int s, t, n, fd;
    unsigned long nr = 0;

    s = open(p->name, O_RDWR);
    if (s < 0) {
        return 1;
    }
    if (tcgetsid(s) != getsid(0)) {
        return 2;                       /* not this session's */
    }
    if (tcgetpgrp(s) != getpgrp()) {
        return 3;                       /* the leader's group in front */
    }
    t = open("/dev/tty", O_RDWR);
    if (t < 0) {
        return 4;
    }
    if (write(t, "via-dev-tty\n", 12) != 12) {
        return 5;
    }
    /* /proc says which terminal: tty_nr is the 7th field. */
    fd = open("/proc/self/stat", O_RDONLY);
    n = fd < 0 ? -1 : (int)read(fd, stat, sizeof(stat) - 1);
    if (n > 0) {
        char *q = strrchr(stat, ')');
        int f;

        stat[n] = '\0';
        for (f = 3; q && f <= 7; f++) {
            q = strchr(q + 1, ' ');
            if (f == 7 && q) {
                nr = strtoul(q + 1, 0, 10);
            }
        }
    }
    snprintf(buf, sizeof(buf), "%s", p->name + 9);     /* "/dev/pts/N" */
    if (nr != ((136UL << 8) | strtoul(buf, 0, 10))) {
        return 6;
    }
    return 0;
}

/* --- 3. a child inherits it; another session cannot use it ----------- */
static int t_inherit(void *v)
{
    struct pt *p = v;
    int s = open(p->name, O_RDWR), st = 0;
    pid_t c;

    if (s < 0) {
        return 1;
    }
    c = fork();
    if (c == 0) {
        int t;

        setpgid(0, 0);
        t = open("/dev/tty", O_RDWR);
        /* a job of the session may be put in front, by the session */
        _exit(t >= 0 ? 0 : 1);
    }
    waitpid(c, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return 2;
    }
    /* Another session: it opens the same terminal, but it is not
     * theirs -- no TIOCSPGRP, and /dev/tty is nothing for them. */
    c = fork();
    if (c == 0) {
        int fd, pg;

        if (setsid() < 0) {
            _exit(9);
        }
        fd = open(p->name, O_RDWR | O_NOCTTY);
        pg = getpgrp();
        errno = 0;
        if (fd < 0 || ioctl(fd, TIOCSPGRP, &pg) == 0 || errno != ENOTTY) {
            _exit(3);
        }
        errno = 0;
        if (open("/dev/tty", O_RDWR) >= 0 || errno != ENXIO) {
            _exit(4);
        }
        _exit(tcgetsid(fd) == getsid(getppid()) ? 0 : 5);
    }
    waitpid(c, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 6;
}

/* --- 4. O_NOCTTY ------------------------------------------------------ */
static int t_noctty(void *v)
{
    struct pt *p = v;
    int s = open(p->name, O_RDWR | O_NOCTTY);

    if (s < 0) {
        return 1;
    }
    errno = 0;
    if (tcgetsid(s) != -1 || errno != ENOTTY) {
        return 2;                       /* it should control nothing */
    }
    errno = 0;
    return (open("/dev/tty", O_RDWR) < 0 && errno == ENXIO) ? 0 : 3;
}

/* --- 5. TIOCSCTTY: refused to a user, taken by root ------------------- */
static int t_steal(void *v)
{
    struct pt *p = v;
    int s = open(p->name, O_RDWR | O_NOCTTY), st = 0;
    pid_t c;

    if (s < 0) {
        return 1;
    }
    c = fork();
    if (c == 0) {
        if (setuid(65534) != 0) {
            _exit(8);
        }
        errno = 0;
        _exit(ioctl(s, TIOCSCTTY, 1) != 0 && errno == EPERM ? 0 : 2);
    }
    waitpid(c, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return 20 + (WIFEXITED(st) ? WEXITSTATUS(st) : 9);
    }
    if (ioctl(s, TIOCSCTTY, 1) != 0) {
        return 3;                       /* root, arg 1: it is ours now */
    }
    return tcgetsid(s) == getsid(0) ? 0 : 4;
}

/* --- 6. hangup: the master closes ------------------------------------- */
static int hup_pipe[2];

/* Say so, and go: a process hung up has nothing left to do, and one
 * left in pause() would outlive the test. */
static void on_hup(int sig)
{
    char c = 'H';

    (void)sig;
    write(hup_pipe[1], &c, 1);
    _exit(0);
}

struct hup { char name[32]; int ready[2]; int master; };

static int t_hangup_leader(void *v)
{
    struct hup *h = v;

    /* The master is the test's to close; a copy here would keep the pty
     * from ever hanging up. */
    close(h->master);
    int s;
    pid_t job;
    char c = 'R';

    signal(SIGHUP, on_hup);
    s = open(h->name, O_RDWR);
    if (s < 0) {
        return 1;
    }
    job = fork();
    if (job == 0) {
        /* The foreground job: its own group, put in front. */
        setpgid(0, 0);
        signal(SIGHUP, on_hup);
        tcsetpgrp(s, getpgrp());
        write(h->ready[1], &c, 1);
        for (;;) {
            pause();
        }
    }
    write(h->ready[1], &c, 1);
    for (;;) {
        pause();
    }
}

/* --- 7. the leader exits: its foreground job is hung up --------------- */
static int t_leader_exits(void *v)
{
    struct hup *h = v;

    /* The master is the test's to close; a copy here would keep the pty
     * from ever hanging up. */
    close(h->master);
    int s = open(h->name, O_RDWR);
    pid_t job;
    char c = 'R';

    if (s < 0) {
        return 1;
    }
    job = fork();
    if (job == 0) {
        setpgid(0, 0);
        signal(SIGHUP, on_hup);
        tcsetpgrp(s, getpgrp());
        write(h->ready[1], &c, 1);
        for (;;) {
            pause();
        }
    }
    {
        char x;

        read(h->ready[0], &x, 1);       /* the job is in front */
    }
    printf("    (leader: front is %d, job %d)\n", (int)tcgetpgrp(s), (int)job);
    return 0;                           /* and the leader goes */
}

/* --- 8. TIOCNOTTY by the leader --------------------------------------- */
static int t_notty(void *v)
{
    struct hup *h = v;

    /* The master is the test's to close; a copy here would keep the pty
     * from ever hanging up. */
    close(h->master);
    int s = open(h->name, O_RDWR);
    pid_t job;
    char c = 'R', x;

    if (s < 0) {
        return 1;
    }
    job = fork();
    if (job == 0) {
        setpgid(0, 0);
        signal(SIGHUP, on_hup);
        tcsetpgrp(s, getpgrp());
        write(h->ready[1], &c, 1);
        for (;;) {
            pause();
        }
    }
    read(h->ready[0], &x, 1);
    if (ioctl(s, TIOCNOTTY, 0) != 0) {
        return 2;
    }
    errno = 0;
    if (open("/dev/tty", O_RDWR) >= 0 || errno != ENXIO) {
        return 3;
    }
    errno = 0;
    return (tcgetsid(s) == -1 && errno == ENOTTY) ? 0 : 4;
}

/* --- 9. setsid gives it up --------------------------------------------- */
static int t_setsid_drops(void *v)
{
    struct pt *p = v;
    int s = open(p->name, O_RDWR), st = 0;
    pid_t c;

    if (s < 0) {
        return 1;
    }
    c = fork();
    if (c == 0) {
        if (setsid() < 0) {
            _exit(2);
        }
        errno = 0;
        _exit(open("/dev/tty", O_RDWR) < 0 && errno == ENXIO ? 0 : 3);
    }
    waitpid(c, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 4;
}

/* Count 'H's arriving on the pipe within about a second. */
static int hups(void)
{
    int n = 0, i;
    char c;

    for (i = 0; i < 20; i++) {
        while (read(hup_pipe[0], &c, 1) == 1) {
            n++;
        }
        usleep(50000);
    }
    return n;
}

static void hup_setup(struct hup *h, int *master)
{
    *master = new_pty(h->name, sizeof(h->name));
    h->master = *master;
    pipe(h->ready);
}

/*
 * A process group whose only member is a ZOMBIE still exists, for
 * setpgid, until the zombie is reaped -- Linux's rule, and what bash
 * relies on when a pipeline's first stage (a builtin) finishes before
 * the later stages join its group. Exit status: 0 good, else which part.
 */
static int zombie_state(pid_t p)
{
    char path[32], buf[128], *c;
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)p);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    c = strrchr(buf, ')');
    return c && c[1] == ' ' && c[2] == 'Z';
}

static int t_zombie_group(void *unused)
{
    pid_t a, b;
    int st, i;

    (void)unused;
    a = fork();
    if (a == 0) {
        setpgid(0, 0);
        _exit(0);
    }
    for (i = 0; i < 500 && !zombie_state(a); i++) {
        usleep(10000);
    }
    if (!zombie_state(a)) {
        return 1;                       /* never saw it a zombie */
    }
    b = fork();
    if (b == 0) {
        _exit(setpgid(0, a) == 0 ? 0 : 1);
    }
    waitpid(b, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        return 2;                       /* refused: the bug */
    }
    waitpid(a, &st, 0);                 /* reaped: the group is gone */
    b = fork();
    if (b == 0) {
        _exit(setpgid(0, a) < 0 && errno == EPERM ? 0 : 1);
    }
    waitpid(b, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 3;
}

int main(void)
{
    struct pt p;
    struct hup h;
    char buf[64];
    int r, m, st;
    pid_t leader;

    setvbuf(stdout, 0, _IONBF, 0);
    pipe(hup_pipe);
    fcntl(hup_pipe[0], F_SETFL, O_NONBLOCK);

    check("a new session has no controlling terminal: /dev/tty is ENXIO",
          in_session(t_no_tty, 0) == 0);

    p.master = new_pty(p.name, sizeof(p.name));
    r = in_session(t_acquire, &p);
    printf("    (acquire: %d)\n", r);
    check("a leader opening a pty gets it: tcgetsid, its group in front, /dev/tty, tty_nr",
          r == 0);
    r = (int)read(p.master, buf, sizeof(buf) - 1);
    buf[r > 0 ? r : 0] = '\0';
    check("  what it wrote to /dev/tty came out of the master", strstr(buf, "via-dev-tty") != 0);
    close(p.master);

    p.master = new_pty(p.name, sizeof(p.name));
    r = in_session(t_inherit, &p);
    printf("    (inherit: %d)\n", r);
    check("a job inherits it; another session can neither take the front nor open /dev/tty",
          r == 0);
    close(p.master);

    p.master = new_pty(p.name, sizeof(p.name));
    check("O_NOCTTY: opened, but it controls nothing", in_session(t_noctty, &p) == 0);
    close(p.master);

    p.master = new_pty(p.name, sizeof(p.name));
    r = in_session(t_acquire, &p);      /* someone else's now */
    r = in_session(t_steal, &p);
    printf("    (steal: %d)\n", r);
    check("TIOCSCTTY on another session's terminal: EPERM for a user, taken by root with 1",
          r == 0);
    close(p.master);

    /* The hangup: a session with a job in front, and the master shut. */
    hup_setup(&h, &m);
    leader = fork();
    if (leader == 0) {
        setsid();
        _exit(t_hangup_leader(&h));
    }
    read(h.ready[0], buf, 1);
    read(h.ready[0], buf, 1);
    hups();                             /* nothing yet */
    close(m);
    r = hups();
    printf("    (hups: %d)\n", r);
    check("the master closed: SIGHUP to the session's leader AND its foreground job", r == 2);
    kill(-leader, SIGKILL);
    waitpid(leader, &st, 0);

    hup_setup(&h, &m);
    leader = fork();
    if (leader == 0) {
        setsid();
        _exit(t_leader_exits(&h));
    }
    waitpid(leader, &st, 0);
    r = hups();
    check("the leader exits: its foreground job is sent SIGHUP", r == 1);
    {
        int s = open(h.name, O_RDWR | O_NOCTTY);

        errno = 0;
        check("  and the terminal belongs to no session any more",
              s >= 0 && tcgetsid(s) == -1 && errno == ENOTTY);
        close(s);
    }
    close(m);

    hup_setup(&h, &m);
    r = in_session(t_notty, &h);
    printf("    (notty: %d)\n", r);
    check("TIOCNOTTY by the leader: the terminal let go, /dev/tty gone", r == 0);
    check("  and the job in front was hung up", hups() == 1);
    close(m);

    p.master = new_pty(p.name, sizeof(p.name));
    check("setsid() leaves the old terminal behind", in_session(t_setsid_drops, &p) == 0);
    close(p.master);

    r = in_session(t_zombie_group, 0);
    printf("    (zombie group: %d)\n", r);
    check("setpgid joins a group whose only member is a zombie; once reaped, EPERM",
          r == 0);

    printf("sesstest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
