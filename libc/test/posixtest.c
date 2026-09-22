/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * posixtest - the POSIX calls added for the standard tools (task 24).
 *
 * uname, gethostname, wait4, wait3 and getrusage, fchdir, the *at calls
 * relative to an open directory, the chown family, fchmod, link, mkfifo,
 * realpath and getdtablesize. Each check compares against a second,
 * independent source of the same fact where there is one: a file made
 * with openat is looked for with stat by its absolute name, a directory
 * reached with fchdir is compared with getcwd, CPU time from wait4 with
 * what the child itself measured.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <stdio_ext.h>
#include <locale.h>
#include <dirent.h>
#include <glob.h>
#include <signal.h>
#include <spawn.h>
#include <utime.h>
#include <sys/random.h>
#include <sys/time.h>

static int fails;

static void report(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        fails++;
    }
}

static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static long ms_of(const struct timeval *tv)
{
    return (long)tv->tv_sec * 1000 + tv->tv_usec / 1000;
}

/* Burn about `ms` of CPU in user mode, measured by the clock. */
static void spin(long ms)
{
    struct timespec a, b;
    volatile unsigned long n = 0;

    clock_gettime(CLOCK_MONOTONIC, &a);
    do {
        for (int i = 0; i < 10000; i++) {
            n++;
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
    } while ((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000 < ms);
}

static void test_machine(void)
{
    struct utsname u;
    char name[64], small[4];

    memset(&u, 0x55, sizeof(u));
    report("uname", uname(&u) == 0);
    report("  names the system, and every field is terminated",
           strcmp(u.sysname, "Sage040") == 0 && strlen(u.release) < sizeof(u.release) &&
           strlen(u.version) < sizeof(u.version) && strlen(u.machine) < sizeof(u.machine));
    report("  the machine is the kernel's answer, a 68040", strcmp(u.machine, "m68040") == 0);

    memset(name, 0, sizeof(name));
    report("gethostname gives uname's node name",
           gethostname(name, sizeof(name)) == 0 && strcmp(name, u.nodename) == 0);
    /* The picolibc bug: it wrote a whole struct utsname into `name`. */
    memset(small, 'Z', sizeof(small));
    errno = 0;
    report("  and refuses a buffer too short, writing nothing past it",
           gethostname(small, 2) < 0 && errno == ENAMETOOLONG &&
           small[2] == 'Z' && small[3] == 'Z');

    {
        struct rlimit rl;

        report("getdtablesize is the descriptor limit",
               getrlimit(RLIMIT_NOFILE, &rl) == 0 && getdtablesize() == (int)rl.rlim_cur &&
               getdtablesize() == 64);
    }
}

static void test_usage(void)
{
    struct rusage ru, before, after;
    int st;
    pid_t pid, got;

    getrusage(RUSAGE_CHILDREN, &before);

    pid = fork();
    if (pid == 0) {
        spin(400);
        _exit(7);
    }
    memset(&ru, 0x55, sizeof(ru));
    got = wait4(pid, &st, 0, &ru);
    report("wait4 reaps the child it names",
           got == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);
    /* 400 ms of spinning is 400 ms of CPU, give or take a tick or two and
     * whatever the machine spent on other things meanwhile. */
    report("  and its rusage has the child's CPU time, in sane units",
           ms_of(&ru.ru_utime) + ms_of(&ru.ru_stime) >= 250 &&
           ms_of(&ru.ru_utime) + ms_of(&ru.ru_stime) <= 800 &&
           ru.ru_utime.tv_usec >= 0 && ru.ru_utime.tv_usec < 1000000);

    getrusage(RUSAGE_CHILDREN, &after);
    report("getrusage(RUSAGE_CHILDREN) grew by the child's time",
           ms_of(&after.ru_utime) + ms_of(&after.ru_stime) -
           ms_of(&before.ru_utime) - ms_of(&before.ru_stime) >= 250);

    getrusage(RUSAGE_SELF, &before);
    spin(300);
    getrusage(RUSAGE_SELF, &after);
    report("getrusage(RUSAGE_SELF) counts this process's own time",
           ms_of(&after.ru_utime) + ms_of(&after.ru_stime) -
           ms_of(&before.ru_utime) - ms_of(&before.ru_stime) >= 200);

    pid = fork();
    if (pid == 0) {
        spin(300);
        _exit(3);
    }
    memset(&ru, 0x55, sizeof(ru));
    got = wait3(&st, 0, &ru);
    report("wait3's rusage too (picolibc read the kernel's layout wrongly)",
           got == pid && WEXITSTATUS(st) == 3 &&
           ms_of(&ru.ru_utime) + ms_of(&ru.ru_stime) >= 150 &&
           ms_of(&ru.ru_utime) + ms_of(&ru.ru_stime) <= 700);

    errno = 0;
    report("getrusage of nobody is EINVAL",
           getrusage(12345, &ru) < 0 && errno == EINVAL);
}

static void test_dirs(void)
{
    char cwd[256];
    struct stat st;
    int d, f, other;

    mkdir("/PT", 0755);
    mkdir("/PT/SUB", 0755);
    chdir("/");

    d = open("/PT/SUB", O_RDONLY | O_DIRECTORY);
    report("a directory opens as a descriptor", d >= 0);

    f = openat(d, "made.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    report("openat relative to it creates the file there",
           f >= 0 && write(f, "hello\n", 6) == 6 && close(f) == 0 &&
           exists("/PT/SUB/made.txt") && !exists("/made.txt"));

    report("fstatat finds it the same way",
           fstatat(d, "made.txt", &st, 0) == 0 && st.st_size == 6 &&
           S_ISREG(st.st_mode));
    report("faccessat too", faccessat(d, "made.txt", R_OK, 0) == 0);
    errno = 0;
    report("  and says ENOENT for what is not there",
           faccessat(d, "nothere", F_OK, 0) < 0 && errno == ENOENT);

    report("AT_FDCWD still means the working directory",
           chdir("/PT") == 0 && fstatat(AT_FDCWD, "SUB/made.txt", &st, 0) == 0);

    report("fchdir moves to the open directory",
           chdir("/") == 0 && fchdir(d) == 0 && getcwd(cwd, sizeof(cwd)) &&
           strcmp(cwd, "/PT/SUB") == 0);
    report("  and relative names then resolve there",
           exists("made.txt"));

    /* The point of resolving at the time of the call: a descriptor
     * follows its directory, as it does on Linux. */
    chdir("/");
    report("a directory renamed while it is open...",
           rename("/PT/SUB", "/PT/MOVED") == 0);
    report("  is still where openat looks",
           fstatat(d, "made.txt", &st, 0) == 0 && st.st_size == 6);
    report("  and where fchdir goes, by its new name",
           fchdir(d) == 0 && getcwd(cwd, sizeof(cwd)) &&
           strcmp(cwd, "/PT/MOVED") == 0);
    chdir("/");

    report("unlinkat removes a file relative to it",
           unlinkat(d, "made.txt", 0) == 0 && !exists("/PT/MOVED/made.txt"));
    mkdirat(d, "inner", 0755);
    errno = 0;
    report("  refuses a directory without AT_REMOVEDIR",
           unlinkat(d, "inner", 0) < 0 && (errno == EISDIR || errno == EPERM));
    report("  and removes it with AT_REMOVEDIR",
           unlinkat(d, "inner", AT_REMOVEDIR) == 0 && !exists("/PT/MOVED/inner"));

    other = open("/PT", O_RDONLY | O_DIRECTORY);
    f = open("/posixtst.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    errno = 0;
    report("a plain file as the directory is ENOTDIR",
           openat(f, "x", O_RDONLY) < 0 && errno == ENOTDIR);
    errno = 0;
    report("a closed descriptor is EBADF",
           openat(99, "x", O_RDONLY) < 0 && errno == EBADF);
    report("an absolute path ignores the descriptor",
           fstatat(other, "/posixtst.tmp", &st, 0) == 0);
    close(f);
    unlink("/posixtst.tmp");

    close(d);
    close(other);
    rmdir("/PT/MOVED");
    rmdir("/PT");
    report("and afterwards everything is gone", !exists("/PT"));
}

static void test_owners(void)
{
    int f = open("/owners.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    report("chown to root, which everything already is",
           chown("/owners.tmp", 0, 0) == 0);
    report("  and to -1, leave it alone", chown("/owners.tmp", (uid_t)-1, (gid_t)-1) == 0);
    errno = 0;
    report("  but to anybody else is EPERM: FAT cannot record it",
           chown("/owners.tmp", 1000, 1000) < 0 && errno == EPERM);
    report("fchown and lchown answer the same",
           fchown(f, 0, 0) == 0 && lchown("/owners.tmp", 0, 0) == 0 &&
           fchown(f, 5, 0) < 0);
    errno = 0;
    report("chown of nothing is ENOENT",
           chown("/nothere.tmp", 0, 0) < 0 && errno == ENOENT);
    report("fchmod of an open file", fchmod(f, 0600) == 0);
    errno = 0;
    report("  and of a closed one is EBADF", fchmod(77, 0600) < 0 && errno == EBADF);
    errno = 0;
    report("link is EPERM: FAT has no hard links",
           link("/owners.tmp", "/owners2.tmp") < 0 && errno == EPERM &&
           !exists("/owners2.tmp"));
    errno = 0;
    report("mkfifo is EPERM: nor FIFOs",
           mkfifo("/fifo.tmp", 0644) < 0 && errno == EPERM && !exists("/fifo.tmp"));
    close(f);
    unlink("/owners.tmp");
}

static void test_realpath(void)
{
    char buf[256], *p;

    mkdir("/RP", 0755);
    mkdir("/RP/A", 0755);
    close(open("/RP/A/file", O_WRONLY | O_CREAT, 0644));
    chdir("/RP");

    report("realpath of a relative name",
           realpath("A/file", buf) && strcmp(buf, "/RP/A/file") == 0);
    report("  with . and .. and doubled slashes",
           realpath("./A/..//A/./file", buf) && strcmp(buf, "/RP/A/file") == 0);
    report("  .. from the root stays at the root",
           realpath("/../../RP", buf) && strcmp(buf, "/RP") == 0);
    report("  the root itself", realpath("/", buf) && strcmp(buf, "/") == 0);
    report("  . is the working directory", realpath(".", buf) && strcmp(buf, "/RP") == 0);
    errno = 0;
    report("  something missing is ENOENT",
           realpath("A/nothere", buf) == NULL && errno == ENOENT);
    errno = 0;
    report("  a file used as a directory is ENOTDIR",
           realpath("A/file/x", buf) == NULL && errno == ENOTDIR);
    p = realpath("A", NULL);
    report("  and with no buffer, one is allocated",
           p && strcmp(p, "/RP/A") == 0);
    free(p);

    chdir("/");
    unlink("/RP/A/file");
    rmdir("/RP/A");
    rmdir("/RP");
}

static void test_memdevs(void)
{
    char buf[64];
    struct stat st;
    int fd, i, zeros = 1;

    fd = open("/dev/null", O_RDWR);
    report("/dev/null opens", fd >= 0);
    report("  reads end at once", read(fd, buf, sizeof(buf)) == 0);
    report("  writes are taken whole", write(fd, "discard me", 10) == 10);
    report("  and it is a character device",
           fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
           stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode));
    close(fd);

    fd = open("/dev/zero", O_RDONLY);
    memset(buf, 0x5a, sizeof(buf));
    report("/dev/zero gives as many zero bytes as asked", read(fd, buf, sizeof(buf)) == 64);
    for (i = 0; i < 64; i++) {
        if (buf[i]) {
            zeros = 0;
        }
    }
    report("  and they are zero", zeros);
    close(fd);

    fd = open("/dev/full", O_WRONLY);
    errno = 0;
    report("/dev/full refuses a write with ENOSPC",
           fd >= 0 && write(fd, "x", 1) < 0 && errno == ENOSPC);
    close(fd);
}

static void test_stdio(void)
{
    FILE *f;
    char buf[16];
    int c1, c2, c3;

    report("stdout on the terminal is line buffered", isatty(1) && __flbf(stdout));

    f = fopen("/stdext.tmp", "w+");
    fputs("abcdef", f);
    report("__fwriting and __fpending after a write",
           __fwriting(f) && !__freading(f) && __fpending(f) == 6);
    report("  and a file is fully buffered, not line buffered", !__flbf(f));
    fflush(f);
    report("  and nothing pending once flushed", __fpending(f) == 0);
    rewind(f);
    c1 = fgetc(f);
    report("__freading and __freadahead after a read",
           c1 == 'a' && __freading(f) && !__fwriting(f) && __freadahead(f) == 5);

    /* Three bytes pushed back, in reverse, as awk does. */
    c1 = fgetc(f); c2 = fgetc(f); c3 = fgetc(f);
    report("ungetc takes back three bytes just read",
           ungetc(c3, f) == 'd' && ungetc(c2, f) == 'c' && ungetc(c1, f) == 'b');
    report("  and they come out again, in order",
           fread(buf, 1, 5, f) == 5 && memcmp(buf, "bcdef", 5) == 0);
    rewind(f);
    fgetc(f);
    report("  a different byte still gets the one-character slot",
           ungetc('Z', f) == 'Z' && fgetc(f) == 'Z' && fgetc(f) == 'b');
    __fpurge(f);
    report("__fpurge discards what was buffered", __freadahead(f) == 0);
    fclose(f);
    unlink("/stdext.tmp");
}

static void test_locale(void)
{
    setenv("LANG", "C.UTF-8", 1);
    unsetenv("LC_ALL");
    unsetenv("LC_CTYPE");
    report("setlocale(LC_CTYPE, \"\") takes LANG from the environment",
           setlocale(LC_CTYPE, "") && strcmp(setlocale(LC_CTYPE, NULL), "C.UTF-8") == 0 &&
           MB_CUR_MAX > 1);
    setenv("LC_ALL", "C", 1);
    report("  and LC_ALL overrides it",
           setlocale(LC_CTYPE, "") && strcmp(setlocale(LC_CTYPE, NULL), "C") == 0 &&
           MB_CUR_MAX == 1);
    unsetenv("LC_ALL");
    unsetenv("LANG");
    report("  and with nothing set it is C",
           setlocale(LC_CTYPE, "") && strcmp(setlocale(LC_CTYPE, NULL), "C") == 0);
}

static void test_sysconf(const char *argv0)
{
    struct rlimit rl;

    getrlimit(RLIMIT_NOFILE, &rl);
    report("sysconf(_SC_OPEN_MAX) is the real limit, not POSIX's 20",
           sysconf(_SC_OPEN_MAX) == (long)rl.rlim_cur && sysconf(_SC_OPEN_MAX) == 64);
    report("sysconf(_SC_CLK_TCK) is the kernel's 100 Hz", sysconf(_SC_CLK_TCK) == 100);
    report("sysconf(_SC_PAGESIZE)", sysconf(_SC_PAGESIZE) == 4096);
    report("sysconf(_SC_ARG_MAX) is what execve accepts",
           sysconf(_SC_ARG_MAX) > 20000 && sysconf(_SC_ARG_MAX) < 32768);
    errno = 0;
    report("  and a name that is not one is EINVAL", sysconf(-5) == -1 && errno == EINVAL);

    report("program_invocation_name is argv[0]",
           program_invocation_name && strcmp(program_invocation_name, argv0) == 0);
    report("getprogname is its last component",
           strcmp(getprogname(), strrchr(argv0, '/') ? strrchr(argv0, '/') + 1 : argv0) == 0 &&
           strcmp(getprogname(), program_invocation_short_name) == 0);
}

/* ---- task 30: the POSIX gaps -------------------------------------- */

static void test_times(void)
{
    struct stat st;
    struct timespec ts[2];
    struct utimbuf ub;
    struct timeval tv[2];
    int fd;
    time_t now;

    fd = open("/TIMES.TMP", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    close(fd);
    /* 2001-02-03 04:05:06 UTC: an even second, which FAT can hold. */
    ts[0].tv_sec = ts[1].tv_sec = 981173106;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    report("utimensat sets a file's modification time",
           utimensat(AT_FDCWD, "/TIMES.TMP", ts, 0) == 0 &&
           stat("/TIMES.TMP", &st) == 0 && st.st_mtime == 981173106);
    ts[1].tv_nsec = UTIME_OMIT;
    ts[0].tv_nsec = UTIME_NOW;
    report("  UTIME_OMIT leaves it alone",
           utimensat(AT_FDCWD, "/TIMES.TMP", ts, 0) == 0 &&
           stat("/TIMES.TMP", &st) == 0 && st.st_mtime == 981173106);
    now = time(NULL);
    report("  a null time means now",
           utimensat(AT_FDCWD, "/TIMES.TMP", NULL, 0) == 0 &&
           stat("/TIMES.TMP", &st) == 0 && st.st_mtime >= now - 3 && st.st_mtime <= now + 3);
    ub.actime = ub.modtime = 1000000000;
    report("utime", utime("/TIMES.TMP", &ub) == 0 && stat("/TIMES.TMP", &st) == 0 &&
                    st.st_mtime == 1000000000);
    tv[0].tv_sec = tv[1].tv_sec = 1100000000;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    report("utimes", utimes("/TIMES.TMP", tv) == 0 && stat("/TIMES.TMP", &st) == 0 &&
                     st.st_mtime == 1100000000);

    /* The case that needs care: a file written and not yet closed has
     * its time stamped when its entry is flushed, which must not undo
     * a time set in between. */
    fd = open("/TIMES.TMP", O_WRONLY | O_APPEND);
    write(fd, "more", 4);
    ts[0].tv_sec = ts[1].tv_sec = 981173106;
    ts[0].tv_nsec = ts[1].tv_nsec = 0;
    report("futimens on a file just written, still open",
           futimens(fd, ts) == 0);
    close(fd);
    report("  and closing it does not stamp over that time",
           stat("/TIMES.TMP", &st) == 0 && st.st_mtime == 981173106 && st.st_size == 4);
    /* Left for libctest.sh, which reads its date with mtools. */
}

static void test_sessions(void)
{
    int p[2], st;
    pid_t pid, got = 0, sid = 0, pg = 0;

    /* In a child that has made itself a group leader first, so that the
     * check cannot pass by being skipped. */
    pid = fork();
    if (pid == 0) {
        if (setpgid(0, 0) < 0 || getpgrp() != getpid()) {
            _exit(2);
        }
        errno = 0;
        _exit(setsid() < 0 && errno == EPERM ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    report("setsid is refused to a process-group leader",
           WIFEXITED(st) && WEXITSTATUS(st) == 0);
    pipe(p);
    pid = fork();
    if (pid == 0) {
        pid_t r[3];

        r[0] = setsid();
        r[1] = getsid(0);
        r[2] = getpgrp();
        write(p[1], r, sizeof(r));
        _exit(0);
    }
    {
        pid_t r[3];

        read(p[0], r, sizeof(r));
        got = r[0];
        sid = r[1];
        pg = r[2];
    }
    waitpid(pid, &st, 0);
    close(p[0]);
    close(p[1]);
    report("  a forked child makes a session of its own and leads its group",
           got == pid && sid == pid && pg == pid);
    report("getsid(0) is this process's session", getsid(0) > 0 && getsid(getpid()) == getsid(0));
}

static void test_hostname(void)
{
    char name[65], saved[65];
    struct utsname u;
    char big[80];

    gethostname(saved, sizeof(saved));
    report("sethostname", sethostname("testhost", 8) == 0);
    report("  gethostname and uname both see it",
           gethostname(name, sizeof(name)) == 0 && strcmp(name, "testhost") == 0 &&
           uname(&u) == 0 && strcmp(u.nodename, "testhost") == 0);
    memset(big, 'x', sizeof(big));
    errno = 0;
    report("  a name longer than 64 is EINVAL", sethostname(big, 70) < 0 && errno == EINVAL);
    sethostname(saved, strlen(saved));
}

static long cpu_of(pid_t pid)
{
    struct rusage ru;
    int st;

    wait4(pid, &st, 0, &ru);
    return ms_of(&ru.ru_utime) + ms_of(&ru.ru_stime);
}

static void test_priority(void)
{
    pid_t a, b;
    long ta, tb;

    errno = 0;
    report("getpriority of this process is 0", getpriority(PRIO_PROCESS, 0) == 0 && errno == 0);
    a = fork();
    if (a == 0) {
        int r = nice(5);

        _exit(r == 5 && getpriority(PRIO_PROCESS, 0) == 5 ? 0 : 1);
    }
    {
        int st;

        waitpid(a, &st, 0);
        report("nice(5) makes it 5, and getpriority agrees",
               WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    report("  and a child's nice is not the parent's", getpriority(PRIO_PROCESS, 0) == 0);

    /* The point: two busy processes, nice 0 and nice 10, side by side for
     * two seconds. Their turns are 5 ticks and 1, so the first should
     * get several times the processor. */
    a = fork();
    if (a == 0) {
        spin(2000);
        _exit(0);
    }
    b = fork();
    if (b == 0) {
        nice(10);
        spin(2000);
        _exit(0);
    }
    {
        /* spin() counts wall time, so both run 2 s; what differs is how
         * much of it each had the processor. Measured with wait4. */
        ta = cpu_of(a);
        tb = cpu_of(b);
    }
    printf("posixtest: nice 0 had %ld ms, nice 10 had %ld ms\n", ta, tb);
    report("a nice-10 process gets well under half the processor beside a nice-0 one",
           ta > 0 && tb > 0 && ta >= 2 * tb);
}

static volatile unsigned long handler_sp;
static volatile int handler_flags;

static void on_alt(int sig)
{
    stack_t now;
    int local;

    (void)sig;
    handler_sp = (unsigned long)&local;
    sigaltstack(NULL, &now);
    handler_flags = now.ss_flags;
}

static void test_altstack(void)
{
    static char alt[16384];
    stack_t ss, old;
    struct sigaction sa;
    int local;

    ss.ss_sp = alt;
    ss.ss_size = sizeof(alt);
    ss.ss_flags = 0;
    report("sigaltstack installs a stack", sigaltstack(&ss, &old) == 0 &&
                                           (old.ss_flags & SS_DISABLE));
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alt;
    sa.sa_flags = SA_ONSTACK;
    report("  and SA_ONSTACK is accepted", sigaction(SIGUSR1, &sa, NULL) == 0);
    raise(SIGUSR1);
    report("  the handler ran on it",
           handler_sp >= (unsigned long)alt && handler_sp < (unsigned long)alt + sizeof(alt));
    report("  and knew it was on it (SS_ONSTACK)", handler_flags == SS_ONSTACK);
    report("  and returned to the ordinary stack",
           (unsigned long)&local < (unsigned long)alt ||
           (unsigned long)&local >= (unsigned long)alt + sizeof(alt));
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    handler_sp = 0;
    raise(SIGUSR1);
    report("  a handler without SA_ONSTACK stays on the ordinary stack",
           handler_sp && (handler_sp < (unsigned long)alt ||
                          handler_sp >= (unsigned long)alt + sizeof(alt)));
    ss.ss_size = 100;
    errno = 0;
    report("  a stack under MINSIGSTKSZ is ENOMEM", sigaltstack(&ss, NULL) < 0 && errno == ENOMEM);
    ss.ss_flags = SS_DISABLE;
    report("  SS_DISABLE removes it",
           sigaltstack(&ss, NULL) == 0 && sigaltstack(NULL, &old) == 0 &&
           (old.ss_flags & SS_DISABLE));
    signal(SIGUSR1, SIG_DFL);
}

static void test_random(void)
{
    unsigned char a[256], b[256];
    int fd, i, ones = 0, same = 0;
    ssize_t n;

    n = getrandom(a, sizeof(a), 0);
    report("getrandom gives what was asked for", n == (ssize_t)sizeof(a));
    report("  twice, differently", getrandom(b, sizeof(b), 0) == (ssize_t)sizeof(b) &&
                                   memcmp(a, b, sizeof(a)) != 0);
    for (i = 0; i < 256; i++) {
        unsigned v = a[i];

        while (v) {
            ones += (int)(v & 1);
            v >>= 1;
        }
        same += a[i] == b[i];
    }
    /* 2048 bits: a fair source is within 1024 +- 150 all but never. */
    report("  and its bits are about half ones", ones > 874 && ones < 1174);
    report("  and two draws agree about as often as chance says", same < 16);
    report("GRND_NONBLOCK once the pool is ready", getrandom(a, 16, GRND_NONBLOCK) == 16);
    fd = open("/dev/urandom", O_RDONLY);
    report("/dev/urandom reads", fd >= 0 && read(fd, a, 64) == 64);
    close(fd);
    fd = open("/dev/random", O_RDONLY);
    report("/dev/random reads", fd >= 0 && read(fd, b, 64) == 64 && memcmp(a, b, 64) != 0);
    report("  and takes a write, mixed in", write(fd, "seed", 4) == 4);
    close(fd);
}

static int count_of(glob_t *g, const char *want)
{
    int i;

    for (i = 0; i < (int)g->gl_pathc; i++) {
        if (strcmp(g->gl_pathv[i], want) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_glob(void)
{
    glob_t g;
    int r;

    mkdir("/GL", 0755);
    mkdir("/GL/sub", 0755);
    close(open("/GL/b.c", O_WRONLY | O_CREAT, 0644));
    close(open("/GL/a.c", O_WRONLY | O_CREAT, 0644));
    close(open("/GL/a.h", O_WRONLY | O_CREAT, 0644));
    close(open("/GL/.hidden.c", O_WRONLY | O_CREAT, 0644));
    close(open("/GL/sub/x.c", O_WRONLY | O_CREAT, 0644));
    close(open("/GL/q[x].c", O_WRONLY | O_CREAT, 0644));  /* FAT forbids * */

    r = glob("/GL/*.c", 0, NULL, &g);
    report("glob finds what matches, sorted, and not the hidden file",
           r == 0 && g.gl_pathc == 3 && strcmp(g.gl_pathv[0], "/GL/a.c") == 0 &&
           strcmp(g.gl_pathv[1], "/GL/b.c") == 0 && !count_of(&g, "/GL/.hidden.c"));
    globfree(&g);
    r = glob("/GL/.*.c", 0, NULL, &g);
    report("  a leading dot in the pattern matches a hidden file",
           r == 0 && g.gl_pathc == 1 && strcmp(g.gl_pathv[0], "/GL/.hidden.c") == 0);
    globfree(&g);
    r = glob("/GL/*/*.c", 0, NULL, &g);
    report("  through a directory level", r == 0 && g.gl_pathc == 1 &&
                                          strcmp(g.gl_pathv[0], "/GL/sub/x.c") == 0);
    globfree(&g);
    r = glob("/GL/s*", GLOB_MARK, NULL, &g);
    report("  GLOB_MARK marks a directory", r == 0 && g.gl_pathc == 1 &&
                                            strcmp(g.gl_pathv[0], "/GL/sub/") == 0);
    globfree(&g);
    r = glob("/GL/q\\[x].c", 0, NULL, &g);
    report("  a backslash makes [ literal", r == 0 && g.gl_pathc == 1 &&
                                           strcmp(g.gl_pathv[0], "/GL/q[x].c") == 0);
    globfree(&g);
    r = glob("/GL/*.zz", 0, NULL, &g);
    report("  nothing matching is GLOB_NOMATCH", r == GLOB_NOMATCH);
    r = glob("/GL/*.zz", GLOB_NOCHECK, NULL, &g);
    report("  or the pattern itself with GLOB_NOCHECK",
           r == 0 && g.gl_pathc == 1 && strcmp(g.gl_pathv[0], "/GL/*.zz") == 0);
    globfree(&g);
    g.gl_offs = 2;
    r = glob("/GL/a.*", GLOB_DOOFFS, NULL, &g);
    r |= glob("/GL/b.c", GLOB_DOOFFS | GLOB_APPEND, NULL, &g);
    report("  GLOB_DOOFFS and GLOB_APPEND",
           r == 0 && g.gl_pathc == 3 && g.gl_pathv[0] == NULL && g.gl_pathv[1] == NULL &&
           strcmp(g.gl_pathv[2], "/GL/a.c") == 0 && strcmp(g.gl_pathv[4], "/GL/b.c") == 0 &&
           g.gl_pathv[5] == NULL);
    globfree(&g);
    unlink("/GL/sub/x.c");
    rmdir("/GL/sub");
    unlink("/GL/a.c");
    unlink("/GL/b.c");
    unlink("/GL/a.h");
    unlink("/GL/.hidden.c");
    unlink("/GL/q[x].c");
    rmdir("/GL");
}

static void test_spawn(const char *self)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    char *argv[] = { (char *)self, "spawned", NULL };
    char buf[32];
    pid_t pid;
    int st, r, fd, n;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/SPAWN.OUT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    r = posix_spawn(&pid, self, &fa, &at, argv, NULL);
    report("posix_spawn starts a program", r == 0 && pid > 0);
    waitpid(pid, &st, 0);
    report("  which exits with its own status", WIFEXITED(st) && WEXITSTATUS(st) == 42);
    fd = open("/SPAWN.OUT", O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    report("  with stdout opened by a file action", strstr(buf, "spawned in") != NULL);
    report("  and in a process group of its own", strstr(buf, "own group") != NULL);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    unlink("/SPAWN.OUT");
    argv[0] = "/NOSUCH";
    r = posix_spawn(&pid, "/NOSUCH", NULL, NULL, argv, NULL);
    report("  and a program that is not there is ENOENT from posix_spawn itself", r == ENOENT);
}

static void test_chroot(void)
{
    pid_t pid;
    int st;

    mkdir("/JAIL", 0755);
    mkdir("/JAIL/inner", 0755);
    close(open("/JAIL/flag", O_WRONLY | O_CREAT, 0644));
    pid = fork();
    if (pid == 0) {
        struct stat sb;
        char cwd[64];
        int bad = 0;

        if (chroot("/JAIL") < 0 || chdir("/") < 0)
            _exit(10);
        bad |= stat("/flag", &sb) != 0;                     /* "/" is the jail */
        bad |= (stat("/POSIXTST", &sb) == 0) << 1;          /* the world is not */
        bad |= (stat("/../flag", &sb) != 0) << 2;           /* ".." stays in */
        bad |= (stat("/../../POSIXTST", &sb) == 0) << 3;
        bad |= (chdir("/inner/../..") != 0 || !getcwd(cwd, sizeof(cwd)) ||
                strcmp(cwd, "/") != 0) << 4;
        bad |= (stat("flag", &sb) != 0) << 5;               /* and cd .. did not leave */
        _exit(bad);
    }
    waitpid(pid, &st, 0);
    report("chroot: / is the new root, ..  cannot climb out of it, and nothing outside is visible",
           WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        printf("posixtest: chroot child status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    report("  and the parent's own root is untouched", access("/POSIXTST", F_OK) == 0);
    unlink("/JAIL/flag");
    rmdir("/JAIL/inner");
    rmdir("/JAIL");
}

static void test_misc(void)
{
    int p[2], fd;
    DIR *d;
    char buf[64], tmpl[] = "/MDTXXXXXX";
    struct stat st;
    double la[3];

    report("pipe2 with O_CLOEXEC", pipe2(p, O_CLOEXEC) == 0 &&
                                   (fcntl(p[0], F_GETFD) & FD_CLOEXEC) &&
                                   (fcntl(p[1], F_GETFD) & FD_CLOEXEC));
    fd = dup3(p[0], 20, O_CLOEXEC);
    report("dup3 with O_CLOEXEC", fd == 20 && (fcntl(20, F_GETFD) & FD_CLOEXEC));
    close(20);
    close(p[0]);
    close(p[1]);
    fd = open("/", O_RDONLY | O_DIRECTORY);
    d = fdopendir(fd);
    report("fdopendir, and dirfd gives the descriptor back", d && dirfd(d) == fd && readdir(d));
    closedir(d);
    report("mkdtemp makes a directory of a new name",
           mkdtemp(tmpl) && strncmp(tmpl, "/MDT", 4) == 0 && strcmp(tmpl, "/MDTXXXXXX") &&
           stat(tmpl, &st) == 0 && S_ISDIR(st.st_mode));
    rmdir(tmpl);
    report("confstr(_CS_PATH)", confstr(_CS_PATH, buf, sizeof(buf)) == 5 && strcmp(buf, "/bin") == 0);
    report("fchmodat and fchownat, to what FAT can say",
           fchmodat(AT_FDCWD, "/POSIXTST", 0755, 0) == 0 &&
           fchownat(AT_FDCWD, "/POSIXTST", 0, 0, 0) == 0);
    errno = 0;
    report("mknodat and mkfifoat: EPERM, FAT holds neither",
           mkfifoat(AT_FDCWD, "/F.TMP", 0644) < 0 && errno == EPERM);
    report("sysconf: one processor, and memory in pages",
           sysconf(_SC_NPROCESSORS_ONLN) == 1 && sysconf(_SC_PHYS_PAGES) > 1000 &&
           sysconf(_SC_AVPHYS_PAGES) > 0 &&
           sysconf(_SC_AVPHYS_PAGES) <= sysconf(_SC_PHYS_PAGES));
    report("getloadavg", getloadavg(la, 3) == 3);
    {
        struct timespec now;

        clock_gettime(CLOCK_REALTIME, &now);
        report("clock_settime sets the clock (to what it was)",
               clock_settime(CLOCK_REALTIME, &now) == 0);
        errno = 0;
        report("  and refuses any other clock",
               clock_settime(CLOCK_MONOTONIC, &now) < 0 && errno == EINVAL);
    }
    sync();
    report("sync", 1);
    {
        pid_t c = fork();

        if (c == 0) {
            _exit(0);
        }
        report("waitpid with no status to fill in (picolibc wrote through it)",
               waitpid(c, NULL, 0) == c);
    }
}

int main(int argc, char **argv)
{
    char where[256];

    if (argc > 1 && strcmp(argv[1], "spawned") == 0) {
        /* posix_spawn's child: where did stdout go, and which group. */
        printf("spawned in %s group\n", getpgrp() == getpid() ? "its own" : "another");
        return 42;
    }

    /* Where the shell was when it started this: libctest.sh cds first. */
    printf("posixtest: started in %s\n", getcwd(where, sizeof(where)) ? where : "?");
    printf("posixtest: the calls the standard tools need\n");
    test_machine();
    test_usage();
    test_dirs();
    test_owners();
    test_realpath();
    test_memdevs();
    test_stdio();
    test_locale();
    test_sysconf(argc > 0 ? argv[0] : "");
    test_times();
    test_sessions();
    test_hostname();
    test_priority();
    test_altstack();
    test_random();
    test_glob();
    test_spawn(argc > 0 && argv[0][0] == '/' ? argv[0] : "/POSIXTST");
    test_chroot();
    test_misc();
    printf("posixtest: %d failed\n", fails);
    printf("posixtest: done\n");
    return fails ? 1 : 0;
}
