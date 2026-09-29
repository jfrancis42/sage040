/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * procfstest - /proc, checked against what the program knows for
 * itself.
 *
 * Driven by kernel/procfstest.sh, which runs it as
 *
 *     /bin/procfstest "two words" three
 *
 * Every check compares /proc with a DIFFERENT source of the same fact:
 * the pid with getpid(), the command line with argv, the environment
 * with environ, the executable's inode with stat() of the path, the
 * memory figures with sysinfo(), where main() and a local variable are
 * with the maps lines that should hold them. A /proc that made its
 * answers up consistently with itself would pass a test that only read
 * /proc; it cannot pass these.
 *
 * Prints "  ok   what" or "  FAIL what" for each, then a last line.
 */
#include "ulib.h"

static int failures, checks;

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
    checks++;
    if (!ok) {
        failures++;
    }
}

/* --- small helpers -------------------------------------------------- */

/* ulib has no strcpy. */
static char *strcpy(char *d, const char *s)
{
    char *r = d;

    while ((*d++ = *s++)) {
    }
    return r;
}

static char *utoa(u32 v, char *out)
{
    char tmp[12];
    int n = 0, i = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
    return out;
}

static int startswith(const char *s, const char *p)
{
    while (*p) {
        if (*s++ != *p++) {
            return 0;
        }
    }
    return 1;
}

static const char *strstr_(const char *h, const char *n)
{
    for (; *h; h++) {
        if (startswith(h, n)) {
            return h;
        }
    }
    return 0;
}

static u32 atou(const char *s)
{
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s++ - '0');
    }
    return v;
}

static u32 hexu(const char *s)
{
    u32 v = 0;

    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 16 + (u32)(*s - '0');
        } else if (*s >= 'a' && *s <= 'f') {
            v = v * 16 + (u32)(*s - 'a' + 10);
        } else {
            return v;
        }
    }
}

/* The whole of a file, NUL-terminated; its length, or a negated errno. */
static char big[16384];

static s32 slurp(const char *path, char *buf, u32 size)
{
    int fd = open(path, O_RDONLY);
    u32 n = 0;

    if (fd < 0) {
        return fd;
    }
    for (;;) {
        s32 r = read(fd, buf + n, size - 1 - n);

        if (r < 0) {
            close(fd);
            return r;
        }
        if (r == 0 || n + (u32)r >= size - 1) {
            n += (u32)r;
            break;
        }
        n += (u32)r;
    }
    close(fd);
    buf[n] = '\0';
    return (s32)n;
}

static s32 rl(const char *path, char *out, u32 size)
{
    s32 n = syscall(__NR_readlink, (u32)path, (u32)out, size - 1);

    out[n < 0 ? 0 : n] = '\0';
    return n;
}

/*
 * Field k of a /proc/<pid>/stat line, counting from 1 as proc(5) does.
 * Field 2 is "(comm)" and may hold spaces, so the counting starts after
 * the LAST ')'.
 */
static const char *stat_field(const char *line, int k)
{
    const char *p = line, *close_ = 0;
    int f = 3;

    for (; *p; p++) {
        if (*p == ')') {
            close_ = p;
        }
    }
    if (!close_) {
        return "";
    }
    p = close_ + 2;
    while (f < k && *p) {
        if (*p == ' ') {
            f++;
        }
        p++;
    }
    return p;
}

/* Does the directory list `name`? getdents64 through the kernel, the
 * way a C library's readdir does it. */
static int dir_has(const char *path, const char *name)
{
    static u8 buf[2048];
    int fd = open(path, O_RDONLY | O_DIRECTORY), found = 0;
    s32 n;

    if (fd < 0) {
        return fd;
    }
    while ((n = syscall(__NR_getdents64, (u32)fd, (u32)buf, sizeof(buf))) > 0) {
        s32 off = 0;

        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);

            if (strcmp(d->d_name, name) == 0) {
                found = 1;
            }
            off += d->d_reclen;
        }
    }
    close(fd);
    return found;
}

/* The maps line covering `addr`, copied into `line`; 0 if none. */
static int maps_line(u32 addr, char *line, u32 size)
{
    const char *p = big;

    if (slurp("/proc/self/maps", big, sizeof(big)) < 0) {
        return 0;
    }
    while (*p) {
        const char *e = p;
        u32 lo = hexu(p), hi;

        while (*e && *e != '-') {
            e++;
        }
        hi = hexu(e + 1);
        e = p;
        while (*e && *e != '\n') {
            e++;
        }
        if (addr >= lo && addr < hi) {
            u32 n = (u32)(e - p);

            if (n >= size) {
                n = size - 1;
            }
            memcpy(line, p, n);
            line[n] = '\0';
            return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int ends_with(const char *s, const char *tail)
{
    u32 a = strlen(s), b = strlen(tail);

    return a >= b && strcmp(s + a - b, tail) == 0;
}

/* --- the checks ------------------------------------------------------ */

static void identity(int argc, char **argv)
{
    char want[16], got[256];
    struct stat a, b;
    s32 n;
    int i, ok;

    utoa((u32)getpid(), want);
    rl("/proc/self", got, sizeof(got));
    report("/proc/self is a link to this process's pid", strcmp(got, want) == 0);

    n = slurp("/proc/self/stat", big, sizeof(big));
    report("/proc/self/stat: pid, (comm) and state R",
           n > 0 && atou(big) == (u32)getpid() &&
           startswith(strstr_(big, " ("), " (procfstest) R "));
    report("  and the parent is getppid()'s",
           n > 0 && atou(stat_field(big, 4)) == (u32)getppid());
    report("  and it has Linux's fifty-two fields, no more",
           n > 0 && *stat_field(big, 52) && !strstr_(stat_field(big, 52), " "));

    n = slurp("/proc/self/comm", got, sizeof(got));
    report("/proc/self/comm is the name, newline-terminated",
           n > 0 && strcmp(got, "procfstest\n") == 0);

    n = slurp("/proc/self/status", big, sizeof(big));
    {
        char line[64] = "Pid:\t";

        utoa((u32)getpid(), line + 5);
        report("/proc/self/status names the same pid",
               n > 0 && strstr_(big, line) != 0 &&
               strstr_(big, "Name:\tprocfstest\n") != 0);
    }

    rl("/proc/self/exe", got, sizeof(got));
    report("/proc/self/exe is the program's path", strcmp(got, "/bin/procfstest") == 0);
    ok = stat("/proc/self/exe", &a) == 0 && stat("/bin/procfstest", &b) == 0 &&
         a.st_ino == b.st_ino && a.st_ino != 0;
    report("  and stat() through it is the program's own inode", ok);
    ok = syscall(__NR_lstat, (u32)"/proc/self/exe", (u32)&a) == 0 &&
         S_ISLNK(a.st_mode);
    report("  and lstat() says it is a link", ok);

    /* argv, NUL after each: compared byte for byte. */
    n = slurp("/proc/self/cmdline", big, sizeof(big));
    {
        u32 at = 0;

        ok = n > 0;
        for (i = 0; ok && i < argc; i++) {
            u32 l = strlen(argv[i]) + 1;

            ok = at + l <= (u32)n && memcmp(big + at, argv[i], l) == 0;
            at += l;
        }
        report("/proc/self/cmdline is argv, NUL after each",
               ok && at == (u32)n && argc == 3);
    }

    n = slurp("/proc/self/environ", big, sizeof(big));
    {
        u32 at = 0;

        ok = n > 0 && environ && environ[0];
        for (i = 0; ok && environ[i]; i++) {
            u32 l = strlen(environ[i]) + 1;

            ok = at + l <= (u32)n && memcmp(big + at, environ[i], l) == 0;
            at += l;
        }
        report("/proc/self/environ is environ, NUL after each",
               ok && at == (u32)n);
    }
}

static const char text[] = "the contents of pft.txt\n";

static void descriptors(void)
{
    char want[64], got[256], name[32];
    int fd, fd2, p[2], i;
    s32 n;

    unlink("/tmp/pft.txt");
    fd = open("/tmp/pft.txt", O_CREAT | O_RDWR);
    write(fd, text, strlen(text));

    strcpy(name, "/proc/self/fd/");
    utoa((u32)fd, name + 14);
    rl(name, got, sizeof(got));
    report("fd/N of an open file is its path", strcmp(got, "/tmp/pft.txt") == 0);

    fd2 = open(name, O_RDONLY);
    n = fd2 >= 0 ? read(fd2, got, sizeof(got) - 1) : -1;
    if (n >= 0) {
        got[n] = '\0';
    }
    report("  and opening fd/N opens the file afresh, from the start",
           n == (s32)strlen(text) && strcmp(got, text) == 0);
    close(fd2);

    /* A relative name is recorded as the absolute one it meant. */
    chdir("/tmp");
    fd2 = open("./../tmp/./pft.txt", O_RDONLY);
    strcpy(name, "/proc/self/fd/");
    utoa((u32)fd2, name + 14);
    rl(name, got, sizeof(got));
    report("  a relative name comes out absolute, with . and .. gone",
           strcmp(got, "/tmp/pft.txt") == 0);
    close(fd2);
    chdir("/");

    pipe(p);
    strcpy(name, "/proc/self/fd/");
    utoa((u32)p[0], name + 14);
    rl(name, got, sizeof(got));
    strcpy(name, "/proc/self/fd/");
    utoa((u32)p[1], name + 14);
    rl(name, want, sizeof(want));
    report("fd/N of a pipe is pipe:[N], the same for both ends",
           startswith(got, "pipe:[") && strcmp(got, want) == 0);
    fd2 = open(name, O_WRONLY);
    n = fd2 >= 0 ? write(fd2, "xyz", 3) : -1;
    close(fd2);
    n = n == 3 ? read(p[0], got, 3) : -1;
    report("  and opening a pipe's fd/N reaches the same pipe",
           n == 3 && memcmp(got, "xyz", 3) == 0);

    /* Descriptor 40, well clear of the one the listing itself opens --
     * which takes the lowest free number, and so would be whatever was
     * closed last. */
    dup2(fd, 40);
    close(fd);
    report("fd/ lists 0, 1, 2 and the open file",
           dir_has("/proc/self/fd", "0") == 1 &&
           dir_has("/proc/self/fd", "2") == 1 &&
           dir_has("/proc/self/fd", "40") == 1);
    close(40);
    report("  and not once it is closed", dir_has("/proc/self/fd", "40") == 0);
    report("  where fd/N is then ENOENT",
           rl("/proc/self/fd/40", got, sizeof(got)) == -ENOENT);
    close(p[0]);
    close(p[1]);

    /* A link in the MIDDLE of a path is followed too. */
    n = slurp("/proc/self/cwd/tmp/pft.txt", got, sizeof(got));
    report("cwd/ leads to the working directory's files",
           n == (s32)strlen(text) && strcmp(got, text) == 0);
    i = chdir("/proc/self/cwd/tmp");
    getcwd(got, sizeof(got));
    report("  and chdir() through it lands in the real directory",
           i == 0 && strcmp(got, "/tmp") == 0);
    chdir("/");
}

static volatile u32 data_word = 0x12345678;

static void memory(void)
{
    char line[160];
    int local = 0;
    u8 *heap, *none, *anon;

    report("maps: main() is in the program's text, r-xp, named",
           maps_line((u32)memory, line, sizeof(line)) &&
           strstr_(line, " r-xp ") && ends_with(line, " /bin/procfstest"));
    report("maps: initialised data is the program's too, rw-p",
           maps_line((u32)&data_word, line, sizeof(line)) &&
           strstr_(line, " rw-p ") && ends_with(line, " /bin/procfstest"));
    report("maps: a local variable is on the [stack], rw-p",
           maps_line((u32)&local, line, sizeof(line)) &&
           strstr_(line, " rw-p ") && ends_with(line, " [stack]"));

    heap = sbrk(8192);
    heap[0] = 1;
    report("maps: memory from sbrk is the [heap]",
           heap != (u8 *)-1 && maps_line((u32)heap + 4096, line, sizeof(line)) &&
           ends_with(line, " [heap]"));

    none = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    anon = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                -1, 0);
    {
        int ok = maps_line((u32)none, line, sizeof(line));

        /* Exactly the page: a line of its own, however its neighbours
         * are mapped. */
        if (ok) {
            u32 lo = hexu(line), hi = hexu(line + 9);

            ok = lo == (u32)none && hi == (u32)none + 4096 &&
                 strstr_(line, " ---p ") != 0;
        }
        report("maps: a PROT_NONE page is ---p, and exactly one page", ok);
    }
    report("maps: an anonymous mapping is rw-p",
           anon != MAP_FAILED && maps_line((u32)anon, line, sizeof(line)) &&
           strstr_(line, " rw-p ") != 0);
}

/* How many lines of meminfo are exactly Linux's 27 characters, ending
 * " kB" -- or -1 if any is not. */
static int kb_lines(const char *q)
{
    int lines = 0;

    while (*q) {
        const char *e = q;

        while (*e && *e != '\n') {
            e++;
        }
        if (e - q != 27 || !startswith(e - 3, " kB")) {
            return -1;
        }
        lines++;
        q = *e ? e + 1 : e;
    }
    return lines;
}

static void machine(void)
{
    struct sysinfo si;
    char want[64];
    const char *p;
    s32 n;
    u32 v;

    sysinfo(&si);

    n = slurp("/proc/meminfo", big, sizeof(big));
    p = strstr_(big, "MemTotal:");
    v = 0;
    /* The figure is right-aligned: skip to its digits. */
    if (p) {
        p += 9;
        while (*p == ' ') {
            p++;
        }
        v = atou(p);
    }
    report("meminfo: MemTotal is sysinfo's total, in kB",
           n > 0 && v == si.totalram * (si.mem_unit / 1024) && v > 0);
    report("  and every line is Linux's shape: name to 16, figure in 8, \" kB\"",
           n > 0 && kb_lines(big) >= 8);

    n = slurp("/proc/uptime", big, sizeof(big));
    v = atou(big);
    sysinfo(&si);
    report("uptime: agrees with sysinfo to within a second",
           n > 0 && (v == si.uptime || v + 1 == si.uptime ||
                     v == si.uptime + 1));

    n = slurp("/proc/loadavg", big, sizeof(big));
    {
        const char *last = big;
        const char *q;

        for (q = big; *q; q++) {
            if (*q == ' ') {
                last = q + 1;
            }
        }
        report("loadavg: five fields, and the last pid is at least ours",
               n > 0 && strstr_(big, "/") != 0 &&
               atou(last) >= (u32)getpid());
    }

    n = slurp("/proc/mounts", big, sizeof(big));
    report("mounts: the root volume, and /proc itself",
           n > 0 && startswith(big, "/dev/hda / ext2 rw 0 0\n") &&
           strstr_(big, "proc /proc proc rw 0 0\n") != 0);

    n = slurp("/proc/cpuinfo", big, sizeof(big));
    report("cpuinfo: a 68040, in Linux/m68k's layout",
           n > 0 && startswith(big, "CPU:\t\t68040\n") &&
           strstr_(big, "FPU:\t\t68040\n") != 0);

    n = slurp("/proc/stat", big, sizeof(big));
    p = strstr_(big, "\nbtime ");
    v = p ? atou(p + 7) : 0;
    sysinfo(&si);
    {
        u32 boot = (u32)time(0) - si.uptime;

        report("stat: btime is now less the uptime, to within two seconds",
               n > 0 && startswith(big, "cpu  ") &&
               v + 2 >= boot && v <= boot + 2);
    }

    utoa((u32)getpid(), want);
    report("/proc lists this process, its parent, and self",
           dir_has("/proc", want) == 1 &&
           dir_has("/proc", utoa((u32)getppid(), want)) == 1 &&
           dir_has("/proc", "self") == 1 && dir_has("/proc", "meminfo") == 1);
}

static void refused(void)
{
    report("opening a /proc file for writing is EACCES",
           open("/proc/uptime", O_WRONLY) == -EACCES);
    report("unlink in /proc is EPERM", unlink("/proc/uptime") == -EPERM);
    report("mkdir in /proc is EACCES", mkdir("/proc/new") == -EACCES);
    report("creating a file in /proc is EACCES",
           open("/proc/new", O_CREAT | O_WRONLY) == -EACCES);
    report("a name /proc does not have is ENOENT",
           open("/proc/nosuch", O_RDONLY) == -ENOENT &&
           open("/proc/999999/stat", O_RDONLY) == -ENOENT);
    report("a path through a /proc file is ENOTDIR",
           open("/proc/self/stat/x", O_RDONLY) == -ENOTDIR);
}

/* Standing in /proc, as sbase's ls, find and du do in every directory
 * they list. */
static void standing(void)
{
    char got[256], want[64];
    s32 n;
    int ok;

    ok = chdir("/proc") == 0 && getcwd(got, sizeof(got)) > 0 &&
         strcmp(got, "/proc") == 0;
    report("chdir(\"/proc\") works, and getcwd says so", ok);
    n = slurp("uptime", got, sizeof(got));
    report("  and a relative name there is /proc's", n > 0 && got[0] >= '0');
    rl("self", got, sizeof(got));
    report("  self too", atou(got) == (u32)getpid());

    ok = chdir("self/fd") == 0 && getcwd(got, sizeof(got)) > 0;
    strcpy(want, "/proc/");
    utoa((u32)getpid(), want + 6);
    strcpy(want + strlen(want), "/fd");
    report("chdir through self: getcwd names the pid, as Linux does",
           ok && strcmp(got, want) == 0);
    report("  and listing \".\" there lists the descriptors",
           dir_has(".", "0") == 1 && dir_has(".", "1") == 1);
    ok = chdir("../../..") == 0 && getcwd(got, sizeof(got)) > 0 &&
         strcmp(got, "/") == 0;
    report("  and .. climbs back out to the volume", ok);
    n = slurp("tmp/pft.txt", got, sizeof(got));
    report("  where relative names are the volume's again",
           n == (s32)strlen(text) && strcmp(got, text) == 0);
    report("chdir into a /proc file is ENOTDIR",
           chdir("/proc/uptime") == -ENOTDIR);
}

/* Another process: stopped, a zombie, reaped -- and seen by a process
 * that is not its user. */
static void others(void)
{
    char path[48], got[256];
    int pid, st, ok;
    s32 n;

    pid = fork();
    if (pid == 0) {
        raise(SIGSTOP);
        exit(7);
    }
    waitpid(pid, &st, WUNTRACED);
    strcpy(path, "/proc/");
    utoa((u32)pid, path + 6);
    strcpy(path + strlen(path), "/stat");
    n = slurp(path, big, sizeof(big));
    report("a stopped child's state is T, and its parent is us",
           n > 0 && startswith(stat_field(big, 3), "T ") &&
           atou(stat_field(big, 4)) == (u32)getpid());

    kill(pid, SIGCONT);
    for (ok = 0; ok < 200; ok++) {
        n = slurp(path, big, sizeof(big));
        if (n > 0 && startswith(stat_field(big, 3), "Z ")) {
            break;
        }
        msleep(10);
    }
    report("  once it has exited and not been waited for, Z",
           n > 0 && startswith(stat_field(big, 3), "Z "));
    waitpid(pid, &st, 0);
    report("  and once reaped, gone", open(path, O_RDONLY) == -ENOENT);

    /* A process that is not root, looking at one that is. */
    pid = fork();
    if (pid == 0) {
        char mine[48];
        int bad = 0;

        strcpy(mine, "/proc/");
        utoa((u32)getppid(), mine + 6);
        if (syscall(__NR_setuid32, 1000) != 0) {
            exit(100);
        }
        strcpy(mine + strlen(mine), "/environ");
        if (open(mine, O_RDONLY) != -EACCES) {
            bad |= 1;
        }
        strcpy(mine + strlen(mine) - 8, "/exe");
        if (rl(mine, got, sizeof(got)) != -EACCES) {
            bad |= 2;
        }
        strcpy(mine + strlen(mine) - 4, "/fd");
        if (open(mine, O_RDONLY) != -EACCES) {
            bad |= 4;
        }
        strcpy(mine + strlen(mine) - 3, "/stat");
        if (slurp(mine, big, sizeof(big)) <= 0) {
            bad |= 8;
        }
        if (slurp("/proc/self/environ", big, sizeof(big)) <= 0) {
            bad |= 16;
        }
        exit(bad);
    }
    waitpid(pid, &st, 0);
    report("another user's environ, exe and fd/ are EACCES; stat is not",
           WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
        puts("         (exit ");
        putdec(WIFEXITED(st) ? (u32)WEXITSTATUS(st) : 999);
        puts(")\n");
    }
}

int main(int argc, char **argv)
{
    identity(argc, argv);
    descriptors();
    memory();
    machine();
    refused();
    standing();
    others();

    puts("procfstest: ");
    putdec((u32)checks);
    puts(" checks, ");
    putdec((u32)failures);
    puts(failures ? " FAILED\n" : " failed\n");
    return failures != 0;
}
