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

int main(int argc, char **argv)
{
    char where[256];

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
    printf("posixtest: %d failed\n", fails);
    printf("posixtest: done\n");
    return fails ? 1 : 0;
}
