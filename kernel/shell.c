/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * shell.c - a console shell.
 *
 * It is a program that happens to be linked into the kernel. Every line
 * below reaches the system through `trap #0` and nothing else: there is
 * no call into the filesystem, the disk or the UART anywhere in this
 * file. That is deliberate and it is checkable -- nothing here includes
 * vfs.h, dev.h or console.h.
 *
 * Which means the day programs run unprivileged, this file moves across
 * the boundary unchanged, and swapping FAT16 for another filesystem or
 * the NS16550A for another UART is invisible to it.
 *
 * The commands are named the way a Linux user expects. The disk format
 * is MS-DOS because the host has to read it; nothing else here needs to
 * be, and DOS command names would buy familiarity with a system nobody
 * has used in thirty years.
 *
 * Output redirection with > and >> is real, which is why there is no
 * "write a file" command: `cat > notes.txt` is how a Unix user would
 * already do it. Errors go to descriptor 2 even when output is
 * redirected, which is the distinction stderr exists to make.
 */
#include "syscall.h"
#include "errno.h"
#include "time.h"
#include "edit.h"
#include "string.h"

#define LINE_MAX    128
#define MAX_ARGS    16
#define IOBUF_SIZE  512
#define OUTBUF_SIZE 256

static char line[LINE_MAX];
/* The line as it was typed, before split() chopped it into pieces. `&`
 * puts this in the job table and `fg` gets it back. */
static char cmdline_saved[LINE_MAX];
static char *argv[MAX_ARGS];
static u8 iobuf[IOBUF_SIZE];

/* ---------------------------------------------------------------- */
/* Output                                                            */
/*                                                                    */
/* Buffered, because a system call per character would make the       */
/* terminal visibly slow and there is nothing to be gained from it.   */
/* ---------------------------------------------------------------- */

static int out_fd = STDOUT_FILENO;
static u8  outbuf[OUTBUF_SIZE];
static u32 outlen;
static int out_error;

static void out_flush(void)
{
    if (outlen > 0) {
        if (sys_write(out_fd, outbuf, outlen) < 0) {
            out_error = 1;
        }
        outlen = 0;
    }
}

static void out_putc(char c)
{
    outbuf[outlen++] = (u8)c;
    if (outlen == sizeof(outbuf)) {
        out_flush();
    }
}

static void out_puts(const char *s)
{
    while (*s) {
        out_putc(*s++);
    }
}

static void out_putdec(u32 v)
{
    char buf[12];
    int i = 0;

    if (v == 0) {
        out_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        out_putc(buf[--i]);
    }
}

/* Right-align a number in a field, so columns line up. */
static void out_putdec_pad(u32 v, int width)
{
    u32 t = v;
    int digits = 1;

    while (t >= 10) {
        t /= 10;
        digits++;
    }
    while (digits++ < width) {
        out_putc(' ');
    }
    out_putdec(v);
}

static void out_put2(u32 v)
{
    out_putc((char)('0' + (v / 10) % 10));
    out_putc((char)('0' + v % 10));
}

static void out_puthex8(u8 v)
{
    static const char hex[] = "0123456789abcdef";

    out_putc(hex[(v >> 4) & 0xf]);
    out_putc(hex[v & 0xf]);
}

static void out_puthex32(u32 v)
{
    out_puthex8((u8)(v >> 24));
    out_puthex8((u8)(v >> 16));
    out_puthex8((u8)(v >> 8));
    out_puthex8((u8)v);
}

/* ---------------------------------------------------------------- */
/* Errors, which go to stderr whatever stdout is doing               */
/* ---------------------------------------------------------------- */

static void err_puts(const char *s)
{
    out_flush();
    sys_write(STDERR_FILENO, s, strlen(s));
}

static void err_report(const char *what, int code)
{
    err_puts(what);
    err_puts(": ");
    err_puts(strerror(code));
    err_puts("\n");
}

static void err_usage(const char *usage)
{
    err_puts("usage: ");
    err_puts(usage);
    err_puts("\n");
}

/* ---------------------------------------------------------------- */
/* Parsing                                                           */
/* ---------------------------------------------------------------- */

/*
 * Split the line in place, pulling out a trailing > or >> redirection.
 * Returns the argument count, or -1 if the redirection has no target.
 */
/*
 * Returns -1 for a malformed redirection and -2 for more arguments than
 * fit. Truncating instead would silently drop the tail of a command --
 * `echo` with too many words printed some of them and looked like it had
 * worked, which is a worse failure than refusing.
 */
static int split(char *s, const char **redir, int *append, int *background)
{
    int argc = 0;

    *redir = 0;
    *append = 0;
    *background = 0;

    while (*s) {
        char *tok;

        while (*s == ' ' || *s == '\t') {
            *s++ = '\0';
        }
        if (*s == '\0') {
            break;
        }

        tok = s;
        while (*s && *s != ' ' && *s != '\t') {
            s++;
        }
        if (*s) {
            *s++ = '\0';
        }

        /*
         * A trailing & asks for the background. Only a whole token and
         * only at the end: `a&b` is a file name on this system, not two
         * commands, because there is nothing to run two commands with.
         */
        if (tok[0] == '&' && tok[1] == '\0') {
            *background = 1;
            continue;
        }

        if (tok[0] == '>') {
            const char *name = tok + 1;

            *append = 0;
            if (*name == '>') {
                *append = 1;
                name++;
            }
            if (*name == '\0') {
                /* "> file" rather than ">file" */
                while (*s == ' ' || *s == '\t') {
                    s++;
                }
                if (*s == '\0') {
                    return -1;
                }
                name = s;
                while (*s && *s != ' ' && *s != '\t') {
                    s++;
                }
                if (*s) {
                    *s++ = '\0';
                }
            }
            *redir = name;
            continue;
        }

        if (argc == MAX_ARGS) {
            return -2;
        }
        argv[argc++] = tok;
    }
    return argc;
}

/*
 * Read exactly `digits` digits and step past them. Strict on purpose:
 * "2026-9-1" is refused rather than guessed at, because the one thing
 * worse than failing to set the clock is setting it to something you did
 * not mean.
 */
static int parse_fixed(const char **p, int digits, u32 *out)
{
    u32 v = 0;
    int i;

    for (i = 0; i < digits; i++) {
        char c = (*p)[i];

        if (c < '0' || c > '9') {
            return -1;
        }
        v = v * 10 + (u32)(c - '0');
    }
    *p += digits;
    *out = v;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Commands                                                          */
/* ---------------------------------------------------------------- */

static void cmd_help(void)
{
    out_puts(
        "ls [-l]              list the directory\n"
        "cat [FILE...]        print files, or the terminal until ctrl-D\n"
        "hd FILE              hex dump, first 256 bytes\n"
        "cp SRC DST           copy a file\n"
        "mv SRC DST           rename a file\n"
        "rm FILE...           remove files\n"
        "stat FILE            size, mode and modification time\n"
        "df                   space used and available\n"
        "echo TEXT            print a line\n"
        "date                 show the date and time\n"
        "date -s DATE [TIME]  set them: YYYY-MM-DD and HH:MM[:SS]\n"
        "uname [-a]           system name, or name and version\n"
        "uptime               how long the machine has been up\n"
        "console [DEV on|off] show or change where console output goes\n"
        "sync                 flush pending writes to the disk\n"
        "jobs                 list stopped and queued jobs\n"
        "fg [%N]              run a stopped or queued job\n"
        "bg [%N]              run one in the background (see below)\n"
        "history              the lines remembered so far\n"
        "halt                 stop the processor\n"
        "\n"
        "Anything else is looked up as a program on the disk and run --\n"
        "`cube` runs CUBE. Programs have no extension: the kernel decides\n"
        "what is executable by looking at the file, not at its name.\n"
        "`shutdown` is a program, and stops the machine rather than just\n"
        "the processor.\n"
        "\n"
        "> FILE and >> FILE redirect output, so `cat > notes.txt` writes\n"
        "a file and `echo more >> notes.txt` adds to it.\n"
        "\n"
        "EDITING, the way bash does it:\n"
        "  ctrl-A / ctrl-E    start and end of the line   (also Home, End)\n"
        "  ctrl-B / ctrl-F    back and forward one        (also the arrows)\n"
        "  ctrl-P / ctrl-N    back and forward in history (also up, down)\n"
        "  ctrl-R / ctrl-S    search the history, backwards and forwards\n"
        "  ctrl-U / ctrl-K    delete to the start, to the end\n"
        "  ctrl-W             delete the word before the cursor\n"
        "  ctrl-D             delete forwards; on an empty line, end input\n"
        "\n"
        "JOBS:\n"
        "  ctrl-C             end the running program\n"
        "  ctrl-Z             stop it; `fg` starts it again\n"
        "  CMD &              queue a job -- but nothing runs in the\n"
        "                     background until there is a scheduler, so\n"
        "                     `fg` is what actually runs it\n");
}

static void print_mode(u32 mode)
{
    out_putc(S_ISDIR(mode) ? 'd' : (S_ISCHR(mode) ? 'c' : '-'));
    out_putc((mode & S_IRUSR) ? 'r' : '-');
    out_putc((mode & S_IWUSR) ? 'w' : '-');
}

static void print_stamp(time_t when)
{
    struct tm t;

    gmtime_r(when, &t);
    out_putdec((u32)(t.tm_year + 1900));
    out_putc('-');
    out_put2((u32)(t.tm_mon + 1));
    out_putc('-');
    out_put2((u32)t.tm_mday);
    out_putc(' ');
    out_put2((u32)t.tm_hour);
    out_putc(':');
    out_put2((u32)t.tm_min);
}

static void cmd_ls(int argc, char **args)
{
    struct dirent de;
    int long_form = (argc > 1 && strcmp(args[1], "-l") == 0);
    int i = 0;
    int col = 0;
    u32 bytes = 0;

    while (sys_getdents(i, &de) == 0) {
        int n;

        if (long_form) {
            print_mode(de.d_mode);
            out_puts("  ");
            for (n = (int)strlen(de.d_name); n < 13; n++) {
                out_putc(' ');
            }
            out_puts(de.d_name);
            out_putdec_pad(de.d_size, 10);
            out_puts("  ");
            print_stamp(de.d_mtime);
            out_putc('\n');
        } else {
            out_puts(de.d_name);
            for (n = (int)strlen(de.d_name); n < 14; n++) {
                out_putc(' ');
            }
            if (++col == 5) {
                out_putc('\n');
                col = 0;
            }
        }
        bytes += de.d_size;
        i++;
    }

    if (!long_form) {
        if (col != 0) {
            out_putc('\n');
        }
        return;
    }

    out_putdec((u32)i);
    out_puts(i == 1 ? " file, " : " files, ");
    out_putdec(bytes);
    out_puts(" bytes\n");
}

/*
 * cat with no operands copies the terminal until end of input, which is
 * what makes `cat > notes.txt` a way to write a file. The terminal
 * driver is in canonical mode, so a read returns one line and returns
 * zero when ctrl-D ends it -- exactly as on Linux, and for the same
 * reason: the line discipline belongs in the tty, not in every program.
 */
static void cat_stdin(void)
{
    for (;;) {
        s32 n = sys_read(STDIN_FILENO, iobuf, sizeof(iobuf));
        s32 i;

        if (n <= 0) {
            return;
        }
        for (i = 0; i < n; i++) {
            out_putc((char)iobuf[i]);
        }
    }
}

static int cat_file(const char *name)
{
    int fd = sys_open(name, O_RDONLY);
    s32 n;

    if (fd < 0) {
        err_report(name, fd);
        return -1;
    }
    while ((n = sys_read(fd, iobuf, sizeof(iobuf))) > 0) {
        s32 i;

        for (i = 0; i < n; i++) {
            out_putc((char)iobuf[i]);
        }
    }
    if (n < 0) {
        err_report(name, (int)n);
    }
    sys_close(fd);
    return (n < 0) ? -1 : 0;
}

static void cmd_cat(int argc, char **args)
{
    int i;

    if (argc == 1) {
        cat_stdin();
        return;
    }
    for (i = 1; i < argc; i++) {
        cat_file(args[i]);
    }
}

static void cmd_hexdump(const char *name)
{
    int fd = sys_open(name, O_RDONLY);
    s32 n, i;
    u32 base;

    if (fd < 0) {
        err_report(name, fd);
        return;
    }
    n = sys_read(fd, iobuf, 256);
    if (n < 0) {
        err_report(name, (int)n);
        sys_close(fd);
        return;
    }
    for (base = 0; (s32)base < n; base += 16) {
        out_puthex32(base);
        out_puts("  ");
        for (i = 0; i < 16; i++) {
            if ((s32)(base + i) < n) {
                out_puthex8(iobuf[base + i]);
                out_putc(' ');
            } else {
                out_puts("   ");
            }
        }
        out_putc('|');
        for (i = 0; i < 16 && (s32)(base + i) < n; i++) {
            u8 c = iobuf[base + i];

            out_putc((c >= 32 && c < 127) ? (char)c : '.');
        }
        out_puts("|\n");
    }
    sys_close(fd);
}

static void cmd_cp(const char *from, const char *to)
{
    int in, out;
    s32 n;

    in = sys_open(from, O_RDONLY);
    if (in < 0) {
        err_report(from, in);
        return;
    }
    out = sys_open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        err_report(to, out);
        sys_close(in);
        return;
    }
    while ((n = sys_read(in, iobuf, sizeof(iobuf))) > 0) {
        s32 w = sys_write(out, iobuf, (u32)n);

        if (w < 0) {
            err_report(to, (int)w);
            break;
        }
        if (w != n) {
            err_report(to, -ENOSPC);
            break;
        }
    }
    if (n < 0) {
        err_report(from, (int)n);
    }
    sys_close(in);
    sys_close(out);
}

static void cmd_stat(const char *name)
{
    struct stat st;
    int err = sys_stat(name, &st);

    if (err < 0) {
        err_report(name, err);
        return;
    }
    out_puts(name);
    out_puts("\n  size      ");
    out_putdec(st.st_size);
    out_puts(" bytes\n  blocks    ");
    out_putdec(st.st_blocks);
    out_puts("\n  mode      ");
    print_mode(st.st_mode);
    out_puts("\n  modified  ");
    print_stamp(st.st_mtime);
    out_putc('\n');
}

static void cmd_df(void)
{
    struct statfs sf;
    int err = sys_statfs(&sf);
    u32 total, avail, used;
    int n;

    if (err < 0) {
        err_report("df", err);
        return;
    }
    /*
     * Multiply before dividing. Clusters first and kilobytes second
     * loses everything below a megabyte, which on this volume was enough
     * to report a disk with a kernel on it as completely empty.
     */
    total = (sf.f_blocks * sf.f_bsize) / 1024;
    avail = (sf.f_bfree * sf.f_bsize) / 1024;
    used = total - avail;

    out_puts("volume          type   1K-blocks       used      avail  use%\n");
    out_puts(sf.f_label[0] ? sf.f_label : "(none)");
    for (n = (int)strlen(sf.f_label[0] ? sf.f_label : "(none)");
         n < 16; n++) {
        out_putc(' ');
    }
    out_puts(sf.f_type);
    out_putdec_pad(total, 12);
    out_putdec_pad(used, 11);
    out_putdec_pad(avail, 11);
    out_putdec_pad(total ? (used * 100 + total / 2) / total : 0, 5);
    out_puts("%\n");
}

static void cmd_uname(int argc, char **args)
{
    struct utsname u;

    if (sys_uname(&u) < 0) {
        err_puts("uname: failed\n");
        return;
    }
    out_puts(u.sysname);
    if (argc > 1 && strcmp(args[1], "-a") == 0) {
        out_putc(' ');
        out_puts(u.release);
        out_putc(' ');
        out_puts(u.machine);
        out_puts(" built ");
        out_puts(u.version);
    }
    out_putc('\n');
}

/* ---------------------------------------------------------------- */
/* The clock                                                         */
/* ---------------------------------------------------------------- */

static void show_clock(const struct tm *t)
{
    out_puts(weekday_name(t->tm_wday));
    out_puts(", ");
    out_putdec((u32)t->tm_mday);
    out_putc(' ');
    out_puts(month_name(t->tm_mon));
    out_putc(' ');
    out_putdec((u32)(t->tm_year + 1900));
    out_puts("  ");
    out_put2((u32)t->tm_hour);
    out_putc(':');
    out_put2((u32)t->tm_min);
    out_putc(':');
    out_put2((u32)t->tm_sec);
    out_puts(" UTC\n");
}

static int parse_date(const char *p, struct tm *t)
{
    u32 y, m, d;

    if (parse_fixed(&p, 4, &y) != 0 || *p++ != '-' ||
        parse_fixed(&p, 2, &m) != 0 || *p++ != '-' ||
        parse_fixed(&p, 2, &d) != 0 || *p != '\0') {
        return -1;
    }
    t->tm_year = (int)y - 1900;
    t->tm_mon = (int)m - 1;
    t->tm_mday = (int)d;
    return 0;
}

static int parse_time(const char *p, struct tm *t)
{
    u32 h, mi, sec = 0;

    if (parse_fixed(&p, 2, &h) != 0 || *p++ != ':' ||
        parse_fixed(&p, 2, &mi) != 0) {
        return -1;
    }
    if (*p == ':') {
        p++;
        if (parse_fixed(&p, 2, &sec) != 0) {
            return -1;
        }
    }
    if (*p != '\0') {
        return -1;
    }
    t->tm_hour = (int)h;
    t->tm_min = (int)mi;
    t->tm_sec = (int)sec;
    return 0;
}

/*
 * date               show it
 * date -s DATE       set the date, leaving the time alone
 * date -s TIME       set the time, leaving the date alone
 * date -s DATE TIME  set both
 *
 * Which of the two a single argument is comes from its shape, not from
 * another flag: a date has dashes and a time has colons, and nothing is
 * ambiguous between them.
 */
static void cmd_date(int argc, char **args)
{
    struct tm t;
    time_t now;
    int i, set_any = 0, err;

    now = sys_time(0);
    gmtime_r(now, &t);

    if (argc == 1) {
        if (now == 0) {
            err_puts("date: no clock, or it is not keeping time; "
                     "set it with 'date -s YYYY-MM-DD HH:MM:SS'\n");
            return;
        }
        show_clock(&t);
        return;
    }

    if (strcmp(args[1], "-s") != 0 || argc < 3) {
        err_usage("date [-s YYYY-MM-DD] [HH:MM[:SS]]");
        return;
    }

    for (i = 2; i < argc; i++) {
        if (parse_date(args[i], &t) == 0 || parse_time(args[i], &t) == 0) {
            set_any = 1;
        } else {
            err_usage("date -s YYYY-MM-DD HH:MM[:SS]");
            return;
        }
    }
    if (!set_any) {
        err_usage("date -s YYYY-MM-DD HH:MM[:SS]");
        return;
    }
    if (!tm_valid(&t)) {
        err_puts("date: not a date (check the day against the month)\n");
        return;
    }

    now = timegm(&t);
    err = sys_stime(&now);
    if (err < 0) {
        err_report("date", err);
        return;
    }

    /* Read it back rather than echo what was asked for: the weekday is
     * derived, and a clock that did not take the setting should say so
     * here and not at the next file write. */
    gmtime_r(sys_time(0), &t);
    show_clock(&t);
}

/* ---------------------------------------------------------------- */
/* Where the console goes                                            */
/* ---------------------------------------------------------------- */

/*
 * Console output goes to every enabled sink at once -- the screen and
 * the serial line both -- and input is taken from every source. So this
 * does not move the shell anywhere; it turns one of the places output
 * appears on or off.
 *
 * That is the useful shape for a machine like this. The serial log stays
 * complete whatever the screen is doing, which is exactly when you want
 * it, and the test harnesses drive the machine over the wire with the
 * display switched off entirely.
 */
static void list_console(int which, const char *heading)
{
    struct console_info ci;
    int i;

    out_puts(heading);
    for (i = 0; ; i++) {
        ci.which = which;
        ci.index = i;
        if (sys_ioctl(STDIN_FILENO, TIOCGCONS, (u32)&ci) < 0) {
            break;
        }
        out_puts("  ");
        out_puts(ci.name);
        if (which == CONS_SINK) {
            out_puts(ci.enabled ? "   on\n" : "   off\n");
        } else {
            out_putc('\n');
        }
    }
}

static void cmd_console(int argc, char **args)
{
    struct console_set cs;
    int on, err, i;

    if (argc == 1) {
        list_console(CONS_SINK, "output to:\n");
        list_console(CONS_SOURCE, "input from:\n");
        out_puts("\nturn one off with `console NAME off`\n");
        return;
    }

    if (argc < 3) {
        err_usage("console [NAME on|off]");
        return;
    }

    if (strcmp(args[2], "on") == 0) {
        on = 1;
    } else if (strcmp(args[2], "off") == 0) {
        on = 0;
    } else {
        err_usage("console [NAME on|off]");
        return;
    }

    for (i = 0; i < (int)sizeof(cs.name); i++) {
        cs.name[i] = '\0';
    }
    strncpy(cs.name, args[1], sizeof(cs.name) - 1);
    cs.on = on;

    /*
     * Flushed first, because the very next thing that happens may be
     * the device this output was going to being switched off.
     */
    out_flush();
    err = sys_ioctl(STDIN_FILENO, TIOCSCONS, (u32)&cs);
    if (err == -EBUSY) {
        err_puts("console: that is the only one left\n");
    } else if (err < 0) {
        err_report(args[1], err);
    }
}

/* ---------------------------------------------------------------- */
/* Jobs                                                              */
/* ---------------------------------------------------------------- */

/*
 * `jobs`, `fg` and `bg`, over one system call.
 *
 * What can honestly be done today is less than the vocabulary suggests,
 * and the commands say so rather than pretending. ctrl-Z stops a
 * program and `fg` resumes it, which works. The background does not:
 * running a job while the shell also runs needs something to schedule
 * them, and there is nothing. So `&` records the job and leaves it
 * ready, and `bg` explains itself.
 */
static const char *state_word(int state)
{
    switch (state) {
    case JOB_S_NEW:     return "queued ";
    case JOB_S_RUNNING: return "running";
    case JOB_S_STOPPED: return "stopped";
    default:            return "done   ";
    }
}

static void cmd_jobs(void)
{
    struct job_info info;
    int i, any = 0;

    for (i = 0; sys_jobctl(JOBCTL_INFO, i, &info) == 0; i++) {
        any = 1;
        out_putc('[');
        out_putdec((u32)info.id);
        out_puts("]  ");
        out_puts(state_word(info.state));
        out_puts("  ");
        out_puts(info.cmd);
        if (info.state == JOB_S_DONE && info.status != 0) {
            out_puts("  (exit ");
            out_putdec((u32)info.status);
            out_putc(')');
        }
        out_putc('\n');
    }
    if (!any) {
        out_puts("no jobs\n");
    }
    sys_jobctl(JOBCTL_REAP, 0, 0);
}

/* `fg %2`, `fg 2` or `fg` for the most recent one worth resuming. */
static int job_arg(int argc, char **args)
{
    struct job_info info;
    int i, best = 0;

    if (argc > 1) {
        const char *p = args[1];
        int n = 0;

        if (*p == '%') {
            p++;
        }
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (*p++ - '0');
        }
        return *p == '\0' ? n : 0;
    }
    /* No argument: the highest-numbered job that could be run. */
    for (i = 0; sys_jobctl(JOBCTL_INFO, i, &info) == 0; i++) {
        if (info.state == JOB_S_STOPPED || info.state == JOB_S_NEW) {
            best = info.id;
        }
    }
    return best;
}

static void report_status(const char *what, int status);

/* Find a job by id and copy out what it is. */
static int job_lookup(int id, struct job_info *out)
{
    int i;

    for (i = 0; sys_jobctl(JOBCTL_INFO, i, out) == 0; i++) {
        if (out->id == id) {
            return 0;
        }
    }
    return -ENOENT;
}

static void run_command(char *cmdline);

static void cmd_fg(int argc, char **args)
{
    struct job_info info;
    int id = job_arg(argc, args);
    int status;

    if (id <= 0 || job_lookup(id, &info) < 0) {
        err_puts("fg: no such job\n");
        return;
    }

    if (info.state == JOB_S_NEW) {
        /*
         * Queued with & and never started, so there is no context to
         * return into -- the command line is all there is, and running
         * it is the ordinary path. The job slot goes first so that the
         * spawn does not trip over its own placeholder.
         */
        char cmdline[LINE_MAX];
        int n;

        strncpy(cmdline, info.cmd, sizeof(cmdline) - 1);
        cmdline[sizeof(cmdline) - 1] = '\0';

        /*
         * Strip the & it was queued with. `fg` means run it in the
         * foreground, and leaving the & on would put it straight back
         * in the table it was just taken out of.
         */
        n = (int)strlen(cmdline);
        while (n > 0 && (cmdline[n - 1] == ' ' || cmdline[n - 1] == '\t')) {
            n--;
        }
        if (n > 0 && cmdline[n - 1] == '&') {
            cmdline[n - 1] = '\0';
        }

        sys_jobctl(JOBCTL_DROP, id, 0);
        out_puts(cmdline);
        out_putc('\n');
        out_flush();
        run_command(cmdline);
        return;
    }

    if (info.state != JOB_S_STOPPED) {
        err_puts("fg: that job is not stopped\n");
        return;
    }

    out_puts(info.cmd);
    out_putc('\n');
    out_flush();

    status = sys_jobctl(JOBCTL_FG, id, 0);
    if (status == SPAWN_STOPPED) {
        return;                 /* it stopped again; already announced */
    }
    if (status < 0) {
        err_report("fg", status);
        return;
    }
    report_status(info.cmd, status);
    sys_jobctl(JOBCTL_REAP, 0, 0);
}

static void cmd_bg(int argc, char **args)
{
    struct job_info info;
    int id = job_arg(argc, args);
    int err;

    if (id <= 0 || job_lookup(id, &info) < 0) {
        err_puts("bg: no such job\n");
        return;
    }
    err = sys_jobctl(JOBCTL_BG, id, 0);
    if (err == -ENOSYS) {
        err_puts("bg: nothing can run in the background yet -- there is\n");
        err_puts("    no scheduler. `fg ");
        err_puts(args[0]);
        err_puts("` will run it.\n");
        return;
    }
    if (err < 0) {
        err_report("bg", err);
    }
}

/* ---------------------------------------------------------------- */

static int need(int argc, int want, const char *usage)
{
    if (argc < want) {
        err_usage(usage);
        return 0;
    }
    return 1;
}

static int redirect(const char *name, int append)
{
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = sys_open(name, flags);

    if (fd < 0) {
        err_report(name, fd);
        return -1;
    }
    out_fd = fd;
    outlen = 0;
    out_error = 0;
    return 0;
}

static void redirect_end(void)
{
    out_flush();
    if (out_fd == STDOUT_FILENO) {
        return;
    }
    if (out_error) {
        err_puts("write failed\n");
    }
    sys_close(out_fd);
    out_fd = STDOUT_FILENO;
}

/*
 * How a command ended, reported the way a shell reports it.
 *
 * Silent on success, and silent on ctrl-C: the terminal already echoed
 * `^C` and the newline, and "interrupt" on the next line as well would
 * be saying the same thing twice. Anything else is worth a word.
 */
static void report_status(const char *what, int status)
{
    if (status == 0 || status == 128 + SIGINT) {
        return;
    }
    err_puts(what);
    if (status > 128 && status < 128 + 32) {
        err_puts(status == 128 + SIGTSTP ? ": stopped\n" : ": killed\n");
        return;
    }
    err_puts(": exited ");
    {
        char n[12];
        int i = 0, v = status;

        if (v == 0) {
            n[i++] = '0';
        }
        while (v > 0) {
            n[i++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (i > 0) {
            char c = n[--i];

            sys_write(STDERR_FILENO, &c, 1);
        }
    }
    err_puts("\n");
}

/*
 * Run one command line.
 *
 * Separate from the read loop because `fg` on a job that was queued with
 * & has a command line and no context -- it has to be able to run one
 * without there being a prompt involved.
 */
static void run_command(char *cmdline)
{
    const char *redir;
    int append;
    int background;
    int argc;
    int err;
    int i;

    {
        argc = split(cmdline, &redir, &append, &background);
        if (argc == -1) {
            err_puts("syntax error: > needs a file name\n");
            return;
        }
        if (argc == -2) {
            err_puts("too many arguments\n");
            return;
        }
        if (argc == 0) {
            return;
        }

        if (redir && redirect(redir, append) != 0) {
            return;
        }

        if (strcmp(argv[0], "help") == 0) {
            cmd_help();

        } else if (strcmp(argv[0], "ls") == 0) {
            cmd_ls(argc, argv);

        } else if (strcmp(argv[0], "cat") == 0) {
            cmd_cat(argc, argv);

        } else if (strcmp(argv[0], "hd") == 0) {
            if (need(argc, 2, "hd FILE")) {
                cmd_hexdump(argv[1]);
            }

        } else if (strcmp(argv[0], "cp") == 0) {
            if (need(argc, 3, "cp SRC DST")) {
                cmd_cp(argv[1], argv[2]);
            }

        } else if (strcmp(argv[0], "mv") == 0) {
            if (need(argc, 3, "mv SRC DST")) {
                err = sys_rename(argv[1], argv[2]);
                if (err < 0) {
                    err_report(argv[1], err);
                }
            }

        } else if (strcmp(argv[0], "rm") == 0) {
            if (need(argc, 2, "rm FILE...")) {
                for (i = 1; i < argc; i++) {
                    err = sys_unlink(argv[i]);
                    if (err < 0) {
                        err_report(argv[i], err);
                    }
                }
            }

        } else if (strcmp(argv[0], "stat") == 0) {
            if (need(argc, 2, "stat FILE")) {
                cmd_stat(argv[1]);
            }

        } else if (strcmp(argv[0], "df") == 0) {
            cmd_df();

        } else if (strcmp(argv[0], "echo") == 0) {
            for (i = 1; i < argc; i++) {
                if (i > 1) {
                    out_putc(' ');
                }
                out_puts(argv[i]);
            }
            out_putc('\n');

        } else if (strcmp(argv[0], "date") == 0) {
            cmd_date(argc, argv);

        } else if (strcmp(argv[0], "uname") == 0) {
            cmd_uname(argc, argv);

        } else if (strcmp(argv[0], "console") == 0) {
            cmd_console(argc, argv);

        } else if (strcmp(argv[0], "uptime") == 0) {
            u32 t = sys_times();
            u32 secs = t / HZ;

            out_putdec(secs / 3600);
            out_putc(':');
            out_put2((secs / 60) % 60);
            out_putc(':');
            out_put2(secs % 60);
            out_puts("  (");
            out_putdec(t);
            out_puts(" ticks at ");
            out_putdec(HZ);
            out_puts(" Hz)\n");

        } else if (strcmp(argv[0], "sync") == 0) {
            err = sys_sync();
            if (err < 0) {
                err_report("sync", err);
            }

        } else if (strcmp(argv[0], "halt") == 0) {
            redirect_end();
            out_puts("halting.\n");
            out_flush();
            sys_reboot(RB_HALT_SYSTEM);

        } else if (strcmp(argv[0], "jobs") == 0) {
            cmd_jobs();

        } else if (strcmp(argv[0], "fg") == 0) {
            cmd_fg(argc, argv);

        } else if (strcmp(argv[0], "bg") == 0) {
            cmd_bg(argc, argv);

        } else if (strcmp(argv[0], "history") == 0) {
            for (i = 0; i < edit_history_count(); i++) {
                out_putdec_pad((u32)i + 1, 4);
                out_puts("  ");
                out_puts(edit_history_nth(i));
                out_putc('\n');
            }

        } else {
            /*
             * Not a builtin, so look for a program of that name. This is
             * where a real shell would walk $PATH; there is one directory
             * on this volume, so the name is the path.
             */
            int status;

            out_flush();

            if (background) {
                /*
                 * Nothing can run while the shell runs, so the job is
                 * recorded and left ready rather than started. Saying so
                 * is better than quietly running it in the foreground,
                 * which is what `&` would otherwise appear to do -- and
                 * the person would find out only from the fact that
                 * their prompt never came back.
                 */
                int id = sys_jobctl(JOBCTL_QUEUE, 0, (void *)cmdline_saved);

                if (id < 0) {
                    err_report("&", id);
                } else {
                    out_putc('[');
                    out_putdec((u32)id);
                    out_puts("]  queued -- nothing runs in the background "
                             "until there is a scheduler; `fg ");
                    out_putdec((u32)id);
                    out_puts("` runs it\n");
                }
                redirect_end();
                return;
            }

            status = sys_spawn(argv[0], argc, argv);

            if (status == SPAWN_STOPPED) {
                /* ctrl-Z. Already announced, and listed by `jobs`. */
            } else if (status == -ENOENT || status == -EINVAL) {
                /*
                 * EINVAL here means the name could not be a file on this
                 * volume at all -- more than eight characters before the
                 * dot, say. From a shell's point of view that is still
                 * just a command that does not exist, and saying
                 * "invalid argument" would send someone looking for an
                 * argument they did not give.
                 */
                err_puts(argv[0]);
                err_puts(": command not found\n");
            } else if (status == -ENOEXEC) {
                err_puts(argv[0]);
                err_puts(": not an executable\n");
            } else if (status == -EBUSY) {
                err_puts(argv[0]);
                err_puts(": a stopped job is holding the program area -- "
                         "finish it with fg first\n");
            } else if (status < 0) {
                err_report(argv[0], status);
            } else {
                report_status(argv[0], status);
                sys_jobctl(JOBCTL_REAP, 0, 0);
            }
        }

        redirect_end();
    }
}

/*
 * The prompt loop.
 *
 * All the editing is in edit.c, above the system call boundary, the way
 * a shell's is. What is left here is what a shell's loop actually is:
 * read a line, remember it, run it.
 */
void shell(void)
{
    for (;;) {
        int n = edit_readline("sage$ ", line, (int)sizeof(line));

        if (n == -EINTR) {
            continue;           /* ctrl-C: a fresh prompt, nothing run */
        }
        if (n < 0) {
            /* End of input on the terminal. There is nowhere to exit to,
             * so start a fresh line and carry on. */
            out_putc('\n');
            out_flush();
            continue;
        }
        if (n == 0) {
            continue;
        }

        /*
         * Kept before split() chops it into pieces, because & needs the
         * whole line to put in the job table and `fg` needs it back.
         */
        strncpy(cmdline_saved, line, sizeof(cmdline_saved) - 1);
        cmdline_saved[sizeof(cmdline_saved) - 1] = '\0';

        edit_history_add(line);
        run_command(line);
    }
}
