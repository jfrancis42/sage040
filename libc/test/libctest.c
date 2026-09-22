/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * libctest - picolibc, on this kernel, doing what C programs do.
 *
 * Written the way a program from elsewhere would be: <stdio.h>,
 * <stdlib.h>, <unistd.h>, nothing from this system's own headers. Each
 * check prints "ok" or "FAIL" and a name, as the other test programs do.
 *
 * Several checks are there because picolibc's numbers are not Linux's.
 * Its SIGUSR1 is 30 where Linux's is 10, and its ENOSYS is 88 where
 * Linux's is 38; libos/linux translates both ways. A check that sends
 * SIGUSR1 and sees the handler run proves the translation, not just the
 * signal -- the kernel only ever sees 10.
 *
 * _GNU_SOURCE, as on Linux, for `environ`. picolibc also hides
 * CLOCK_MONOTONIC without it, where glibc shows it under plain POSIX:
 * its headers only claim _POSIX_MONOTONIC_CLOCK for RTEMS. A port that
 * uses the clock and defines nothing needs -D_GNU_SOURCE.
 */
#define _GNU_SOURCE             /* environ, CLOCK_MONOTONIC: see below */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <termios.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/statvfs.h>
#include <sys/file.h>
#include <sys/ioctl.h>

/* POSIX has programs declare it themselves; picolibc declares it nowhere. */
extern char **environ;

#define TMPFILE "/LCTEST.TXT"

static int fails;

static void report(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        fails++;
    }
}

/* --- startup ------------------------------------------------------- */

static int constructed;

__attribute__((constructor)) static void ctor(void)
{
    constructed = 42;
}

/* --- formatting and conversion ------------------------------------- */

static void test_format(void)
{
    char b[128];

    report("a constructor ran before main", constructed == 42);

    snprintf(b, sizeof(b), "%d %5.2f %s %x %lld %c", -17, 3.14159, "str",
             255, 1234567890123LL, 'Z');
    report("snprintf of int, double, string, hex, long long and char",
           strcmp(b, "-17  3.14 str ff 1234567890123 Z") == 0);
    snprintf(b, sizeof(b), "%g %e %.3g", 0.0001, 12345.678, 2.0 / 3.0);
    report("  and %g and %e", strcmp(b, "0.0001 1.234568e+04 0.667") == 0);
    report("strtod", fabs(strtod("2.5e3", 0) - 2500.0) < 1e-9);
    report("strtol with a base", strtol("-0x7f", 0, 16) == -127);
    {
        int a = 0;
        char w[16];

        report("sscanf", sscanf("12 words", "%d %15s", &a, w) == 2 &&
                         a == 12 && strcmp(w, "words") == 0);
    }
}

/* --- memory -------------------------------------------------------- */

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void test_memory(void)
{
    char *p = malloc(8 * 1024 * 1024);
    int *v, i, ok, key = 700;

    report("malloc of 8 MB", p != 0);
    if (p) {
        memset(p, 0xa5, 8 * 1024 * 1024);
        report("  all of which can be written", (u_char)p[8 * 1024 * 1024 - 1] == 0xa5);
    }
    p = realloc(p, 16 * 1024 * 1024);
    report("realloc to 16 MB keeps the contents",
           p && (u_char)p[8 * 1024 * 1024 - 1] == 0xa5);
    free(p);

    v = calloc(1000, sizeof(int));
    ok = v != 0;
    for (i = 0; ok && i < 1000; i++) {
        ok = v[i] == 0;
    }
    report("calloc clears", ok);
    for (i = 0; v && i < 1000; i++) {
        v[i] = (i * 7919) % 1000;
    }
    qsort(v, 1000, sizeof(int), cmp_int);
    ok = 1;
    for (i = 1; v && i < 1000; i++) {
        ok = ok && v[i - 1] <= v[i];
    }
    report("qsort", ok);
    report("bsearch", v && bsearch(&key, v, 1000, sizeof(int), cmp_int) != 0);
    free(v);
}

/* --- control flow and maths ---------------------------------------- */

static jmp_buf jb;

static void jump_back(int n)
{
    longjmp(jb, n);
}

static void test_control(void)
{
    volatile int reached = 0;
    int r = setjmp(jb);

    if (r == 0) {
        reached = 1;
        jump_back(9);
    }
    report("setjmp and longjmp", r == 9 && reached == 1);

    report("sqrt, on the FPU", fabs(sqrt(2.0) - 1.41421356237) < 1e-9);
    report("sin and cos", fabs(sin(M_PI / 6) - 0.5) < 1e-12 &&
                          fabs(cos(0.0) - 1.0) < 1e-12);
    report("pow and exp and log", fabs(pow(2.0, 10.0) - 1024.0) < 1e-9 &&
                                  fabs(log(exp(3.0)) - 3.0) < 1e-12);
}

/* --- files --------------------------------------------------------- */

static void test_files(void)
{
    FILE *f;
    char line[64];
    long n;
    struct stat st;
    int x = 0, fd;
    char buf[1000];

    f = fopen(TMPFILE, "w");
    report("fopen for writing", f != 0);
    if (!f) {
        return;
    }
    fprintf(f, "first line\n%d %s\n", 42, "second");
    report("fclose flushes and succeeds", fclose(f) == 0);

    f = fopen(TMPFILE, "r");
    report("fopen for reading", f != 0);
    if (f) {
        report("fgets reads a line",
               fgets(line, sizeof(line), f) && strcmp(line, "first line\n") == 0);
        report("fscanf reads fields",
               fscanf(f, "%d %63s", &x, line) == 2 && x == 42 &&
               strcmp(line, "second") == 0);
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        report("fseek and ftell", n == 21);
        rewind(f);
        report("rewind", fgetc(f) == 'f');
        fclose(f);
    }
    report("stat gives the size", stat(TMPFILE, &st) == 0 && st.st_size == 21);
    report("  and says it is a regular file, not executable",
           S_ISREG(st.st_mode) && !(st.st_mode & S_IXUSR));

    memset(buf, 'q', sizeof(buf));
    fd = open(TMPFILE, O_WRONLY | O_APPEND);
    report("open with O_APPEND", fd >= 0);
    report("  and write appends", write(fd, buf, sizeof(buf)) == 1000);
    report("fstat on the descriptor sees the new size",
           fstat(fd, &st) == 0 && st.st_size == 1021);
    close(fd);

    report("O_CREAT|O_EXCL refuses a file that exists",
           open(TMPFILE, O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 &&
           errno == EEXIST);
    report("rename", rename(TMPFILE, "/LCTEST2.TXT") == 0 &&
                     access("/LCTEST2.TXT", F_OK) == 0 &&
                     access(TMPFILE, F_OK) < 0);
    report("unlink", unlink("/LCTEST2.TXT") == 0 &&
                     access("/LCTEST2.TXT", F_OK) < 0);

    errno = 0;
    report("fopen of a file that is not there fails with ENOENT",
           fopen("/NOSUCH.TXT", "r") == 0 && errno == ENOENT);
    report("  and strerror says why",
           strstr(strerror(ENOENT), "No such file") != 0);
    {
        const char *lname = "/A longer name, with Case.txt";
        FILE *lf = fopen(lname, "w");
        DIR *dd;
        struct dirent *de;
        int seen = 0;

        report("a long file name can be created",
               lf && fputs("long\n", lf) >= 0 && fclose(lf) == 0);
        dd = opendir("/");
        while (dd && (de = readdir(dd)) != 0) {
            if (strcmp(de->d_name, lname + 1) == 0) {
                seen = 1;
            }
        }
        if (dd) {
            closedir(dd);
        }
        report("  and readdir gives it back, case and all", seen);
        report("  and stat finds it by another case",
               stat("/a LONGER name, with case.TXT", &st) == 0 &&
               st.st_size == 5);
        report("  and unlink removes it", unlink(lname) == 0 &&
                                          access(lname, F_OK) < 0);
    }
    {
        /*
         * One file, two descriptions: a reader open while a writer
         * truncates and rewrites it sees the new file, as on Linux.
         * The FAT driver used to refuse the writer outright while
         * anything had the file open.
         */
        int r, w;
        char b[16];

        f = fopen("/SHARED.TXT", "w");
        fputs("old contents here\n", f);
        fclose(f);
        r = open("/SHARED.TXT", O_RDONLY);
        w = open("/SHARED.TXT", O_WRONLY | O_TRUNC);
        report("a file open for reading can be opened to write too",
               r >= 0 && w >= 0);
        report("  and after the writer truncated it the reader sees it empty",
               fstat(r, &st) == 0 && st.st_size == 0 &&
               read(r, b, sizeof(b)) == 0);
        write(w, "new\n", 4);
        lseek(r, 0, SEEK_SET);
        memset(b, 0, sizeof(b));
        report("  and then sees what the writer wrote",
               read(r, b, sizeof(b)) == 4 && strcmp(b, "new\n") == 0);
        close(w);
        close(r);

        /* ftruncate, both ways. */
        {
            char big[3000];
            int t = open("/TRUNC.TXT", O_RDWR | O_CREAT | O_TRUNC, 0644), ok, k;

            memset(big, 'x', sizeof(big));
            write(t, big, sizeof(big));
            report("ftruncate shortens a file",
                   ftruncate(t, 10) == 0 && fstat(t, &st) == 0 &&
                   st.st_size == 10);
            report("  and lengthens one", ftruncate(t, 5000) == 0 &&
                                          fstat(t, &st) == 0 &&
                                          st.st_size == 5000);
            lseek(t, 0, SEEK_SET);
            memset(big, 0, sizeof(big));
            read(t, big, 10);
            ok = memcmp(big, "xxxxxxxxxx", 10) == 0;
            {
                /* All 4990 bytes, each of them zero: a count as well as
                 * a value, or a gap that read back as nothing at all
                 * would pass. */
                int total = 0, got;

                while ((got = read(t, big, sizeof(big))) > 0) {
                    for (k = 0; k < got; k++) {
                        ok = ok && big[k] == 0;
                    }
                    total += got;
                }
                ok = ok && total == 4990;
            }
            report("  with zeroes, not whatever the disk held", ok);
            close(t);
            report("truncate by name",
                   truncate("/TRUNC.TXT", 3) == 0 &&
                   stat("/TRUNC.TXT", &st) == 0 && st.st_size == 3);
            t = open("/TRUNC.TXT", O_RDONLY);
            report("ftruncate on a read-only descriptor is refused",
                   ftruncate(t, 0) < 0 && errno == EINVAL);
            close(t);
            unlink("/TRUNC.TXT");
        }

        /* flock: the lock belongs to the open description. */
        r = open("/SHARED.TXT", O_RDONLY);
        w = open("/SHARED.TXT", O_RDONLY);
        report("flock takes an exclusive lock", flock(r, LOCK_EX) == 0);
        report("  and another open of the file is refused it",
               flock(w, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK);
        report("  and a shared lock too",
               flock(w, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK);
        {
            int d = dup(r);

            report("  but a dup shares the lock, and may take it again",
                   flock(d, LOCK_EX | LOCK_NB) == 0);
            close(d);
        }
        report("LOCK_UN lets the other have it",
               flock(r, LOCK_UN) == 0 && flock(w, LOCK_EX | LOCK_NB) == 0);
        close(w);
        report("  and closing the holder releases it",
               flock(r, LOCK_SH | LOCK_NB) == 0);
        close(r);
        unlink("/SHARED.TXT");
    }
    report("mkdir and rmdir", mkdir("/LCDIR", 0755) == 0 &&
                              stat("/LCDIR", &st) == 0 && S_ISDIR(st.st_mode) &&
                              rmdir("/LCDIR") == 0);
}

/* --- directories --------------------------------------------------- */

static void test_dirs(const char *self)
{
    DIR *d = opendir("/");
    struct dirent *e;
    int dot = 0, dotdot = 0, bin = 0, me = 0, zero_ino = 0;
    const char *myname = strrchr(self, '/') ? strrchr(self, '/') + 1 : self;
    struct stat st;

    report("opendir", d != 0);
    while (d && (e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0) dot = 1;
        if (strcmp(e->d_name, "..") == 0) dotdot = 1;
        if (strcmp(e->d_name, "BIN") == 0) bin = 1;
        if (strcasecmp(e->d_name, myname) == 0) me = 1;   /* this program */
        if (e->d_ino == 0) zero_ino = 1;
    }
    if (d) {
        closedir(d);
    }
    report("readdir lists the root, with . and ..", dot && dotdot && bin && me);
    report("  and no entry has inode 0", !zero_ino);
    report("stat of this program says it is executable",
           stat(self, &st) == 0 && (st.st_mode & S_IXUSR));
    report("opening a directory to write is EISDIR",
           open("/BIN", O_WRONLY) < 0 && errno == EISDIR);
    report("O_DIRECTORY on a file is ENOTDIR",
           open(self, O_RDONLY | O_DIRECTORY) < 0 && errno == ENOTDIR);
    {
        struct statvfs sv;

        /* statfs64 is not implemented, and says ENOSYS -- which is 38
         * to the kernel and 88 to picolibc. */
        errno = 0;
        report("an unimplemented call reports picolibc's ENOSYS, "
               "translated from Linux's",
               statvfs("/", &sv) < 0 && errno == ENOSYS);
    }
}

/* --- processes ----------------------------------------------------- */

static int exited_via_atexit;

static void at_exit_handler(void)
{
    printf("libctest: atexit handler ran\n");
}

static void test_processes(const char *self)
{
    int st = 0, p[2];
    pid_t pid;
    char buf[16];

    report("getpid and getppid", getpid() > 0 && getppid() > 0);
    report("getuid is root", getuid() == 0 && geteuid() == 0);

    pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)self, "child", "7", 0 };

        execve(self, argv, environ);
        printf("libctest: execve %s: %s\n", self, strerror(errno));
        fflush(stdout);
        _exit(99);
    }
    report("fork", pid > 0);
    {
        pid_t w = waitpid(pid, &st, 0);
        int ok = w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7;

        report("  and waitpid sees the exec'd child's exit status", ok);
        if (!ok) {
            printf("libctest: waitpid gave %d, status %#x\n", (int)w, st);
        }
    }

    report("pipe", pipe(p) == 0);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "through", 7);
        _exit(0);
    }
    close(p[1]);
    {
        struct pollfd pf = { p[0], POLLIN, 0 };

        report("  poll says there is data", poll(&pf, 1, 2000) == 1 &&
                                            (pf.revents & POLLIN));
    }
    memset(buf, 0, sizeof(buf));
    report("  and read gets what the child wrote",
           read(p[0], buf, sizeof(buf)) == 7 && strcmp(buf, "through") == 0);
    close(p[0]);
    waitpid(pid, &st, 0);

    report("getenv", getenv("PATH") != 0);
    (void)exited_via_atexit;
}

/* --- signals ------------------------------------------------------- */

static volatile sig_atomic_t got_sig, got_info_sig, got_alarm;
static volatile int info_code;

static void on_sig(int sig)
{
    got_sig = sig;
}

static void on_info(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    got_info_sig = sig;
    info_code = si->si_signo;
}

static void on_alarm(int sig)
{
    got_alarm = sig;
}

static void test_signals(void)
{
    struct sigaction sa;
    sigset_t set, old;
    int st;
    pid_t pid;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGUSR1, &sa, 0);
    raise(SIGUSR1);
    report("raise(SIGUSR1) runs the handler with picolibc's number",
           got_sig == SIGUSR1);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, 0);
    kill(getpid(), SIGUSR2);
    report("an SA_SIGINFO handler gets the signal in its siginfo",
           got_info_sig == SIGUSR2 && info_code == SIGUSR2);

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    got_sig = 0;
    raise(SIGUSR1);
    sigpending(&old);
    report("a blocked signal is held, and sigpending says so",
           got_sig == 0 && sigismember(&old, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &set, 0);
    report("  and delivered when unblocked", got_sig == SIGUSR1);

    signal(SIGALRM, on_alarm);
    alarm(1);
    pause();
    report("alarm and pause", got_alarm == SIGALRM);

    pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        pause();
        _exit(0);
    }
    usleep(200000);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    report("a child killed by SIGTERM: WTERMSIG says SIGTERM",
           WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
}

/* --- time and the terminal ----------------------------------------- */

static void test_time(void)
{
    struct timespec a, b;
    struct timeval tv;
    time_t now = time(0);
    struct tm *tm;
    char s[64];

    report("time() is after 2020", now > 1577836800);
    tm = gmtime(&now);
    strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", tm);
    report("gmtime and strftime", tm && strlen(s) == 19 && s[4] == '-');
    report("gettimeofday agrees with time()",
           gettimeofday(&tv, 0) == 0 && tv.tv_sec - now <= 1);

    clock_gettime(CLOCK_MONOTONIC, &a);
    usleep(100000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    {
        long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;

        report("CLOCK_MONOTONIC moves across usleep(100 ms)",
               ms >= 90 && ms < 1000);
    }
}

static void test_tty(void)
{
    struct termios t;
    struct winsize w;

    report("isatty(0)", isatty(0));
    report("tcgetattr", tcgetattr(0, &t) == 0 && (t.c_lflag & ICANON));
    report("tcgetwinsize gives the terminal's size",
           tcgetwinsize(0, &w) == 0 && w.ws_row == 24 && w.ws_col == 80);
    report("isatty on a file says no", !isatty(open("/LIBCTEST", O_RDONLY)));
    {
        /* ioctl() itself, which is what programs call -- neatvi does. */
        struct winsize w2;
        int r;

        memset(&w2, 0, sizeof(w2));
        r = ioctl(0, TIOCGWINSZ, &w2);
        printf("libctest: ioctl TIOCGWINSZ = %d, %dx%d, errno %d\n", r,
               w2.ws_row, w2.ws_col, r < 0 ? errno : 0);
        report("ioctl(TIOCGWINSZ) gives the terminal's size",
               r == 0 && w2.ws_row == 24 && w2.ws_col == 80);
    }
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "child") == 0) {
        return atoi(argv[2]);
    }

    printf("libctest: picolibc on SuckOS\n");
    test_format();
    test_memory();
    test_control();
    test_files();
    /* This program, by the name it was run as: the static and the
     * dynamic builds are two files, and each must exec itself. */
    test_dirs(argv[0][0] == '/' ? argv[0] : "/LIBCTEST");
    test_processes(argv[0][0] == '/' ? argv[0] : "/LIBCTEST");
    test_signals();
    test_time();
    test_tty();

    atexit(at_exit_handler);
    printf("libctest: %d failed", fails);   /* no newline: exit flushes */
    printf("\nlibctest: done\n");
    return fails ? 1 : 0;
}
