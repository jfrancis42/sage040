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
 * Redirection (<, >, >>, 2>, 2>>, 2>&1) and pipelines are real, for
 * builtins and programs alike, which is why there is no "write a file"
 * command: `cat > notes.txt` is how a Unix user would already do it.
 * Errors go to descriptor 2 even when output is redirected, which is the
 * distinction stderr exists to make.
 */
#include "syscall.h"
#include "errno.h"
#include "time.h"
#include "edit.h"
#include "string.h"

#define LINE_MAX    1024
#define MAX_ARGS    16
#define IOBUF_SIZE  512
#define OUTBUF_SIZE 256

static char line[LINE_MAX];
/* The line as it was typed, before split() chopped it into pieces. `&`
 * puts this in the job table and `fg` gets it back. */
static char cmdline_saved[LINE_MAX];

/* sh -e and sh -x. */
static int opt_errexit;
static int opt_xtrace;

/* What the last command returned, for $? and for scripts. */
static int last_status;

/* 1 in /bin/sh, 0 in the machine's own shell: see shell_main(). */
static int shell_is_program;
static char *argv[MAX_ARGS];

/* ---------------------------------------------------------------- */
/* The environment                                                   */
/*                                                                    */
/* A flat array of "NAME=VALUE" strings, which is exactly what a      */
/* program is handed -- so passing it on costs nothing and there is   */
/* no second representation to keep in step. The shell is a task      */
/* like any other and this is its own copy; a child gets a copy of    */
/* this one, and changes on either side do not travel back.           */
/* ---------------------------------------------------------------- */

#define ENV_MAX      16
#define ENV_ENTRY    64

static char env_store[ENV_MAX][ENV_ENTRY];
static char *env[ENV_MAX + 1];
static int  env_count;

static void env_rebuild(void)
{
    int i;

    for (i = 0; i < env_count; i++) {
        env[i] = env_store[i];
    }
    env[env_count] = 0;
}

/* The index of NAME, or -1. Compared up to the '=' so that a lookup of
 * "PATH" does not match "PATHX". */
static int env_find(const char *name)
{
    int i;

    for (i = 0; i < env_count; i++) {
        const char *e = env_store[i];
        const char *n = name;

        while (*n && *e && *e != '=' && *n == *e) {
            n++;
            e++;
        }
        if (!*n && *e == '=') {
            return i;
        }
    }
    return -1;
}

static const char *env_get(const char *name)
{
    int i = env_find(name);
    const char *e;

    if (i < 0) {
        return 0;
    }
    e = env_store[i];
    while (*e && *e != '=') {
        e++;
    }
    return *e == '=' ? e + 1 : "";
}

static int env_set(const char *name, const char *value)
{
    int i = env_find(name);
    u32 n = 0;

    if (i < 0) {
        if (env_count == ENV_MAX) {
            return -ENOSPC;
        }
        i = env_count++;
    }
    while (*name && n + 2 < ENV_ENTRY) {
        env_store[i][n++] = *name++;
    }
    env_store[i][n++] = '=';
    while (*value && n + 1 < ENV_ENTRY) {
        env_store[i][n++] = *value++;
    }
    env_store[i][n] = '\0';
    env_rebuild();
    return 0;
}

static void env_unset(const char *name)
{
    int i = env_find(name);

    if (i < 0) {
        return;
    }
    while (i < env_count - 1) {
        strcpy(env_store[i], env_store[i + 1]);
        i++;
    }
    env_count--;
    env_rebuild();
}
static u8 iobuf[IOBUF_SIZE];

/* ---------------------------------------------------------------- */
/* Output                                                            */
/*                                                                    */
/* Buffered, because a system call per character would make the       */
/* terminal visibly slow and there is nothing to be gained from it.   */
/* ---------------------------------------------------------------- */

static u8  outbuf[OUTBUF_SIZE];
static u32 outlen;
static int out_error;

static void out_flush(void)
{
    if (outlen > 0) {
        if (sys_write(STDOUT_FILENO, outbuf, outlen) < 0) {
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

/*
 * A builtin's exit status. A builtin that reports an error through the
 * two functions below has failed, and its status is 1; one that sets
 * last_status itself (test) says so in status_set. It used to be that
 * no builtin set a status at all -- "cat /nope; echo $?" said 0 -- which
 * nothing noticed until && and || came to depend on it.
 */
static int builtin_failed;
static int status_set;

static void err_report(const char *what, int code)
{
    builtin_failed = 1;
    err_puts(what);
    err_puts(": ");
    err_puts(strerror(code));
    err_puts("\n");
}

static void err_usage(const char *usage)
{
    builtin_failed = 1;
    err_puts("usage: ");
    err_puts(usage);
    err_puts("\n");
}

/* ---------------------------------------------------------------- */
/* Parsing                                                           */
/* ---------------------------------------------------------------- */

/*
 * What a command's descriptors are to be pointed at. Each is a file
 * name, or null for "leave it alone".
 */
struct redirs {
    const char *in;             /* < file                             */
    const char *out;            /* > file, or >> file with out_append */
    int out_append;
    const char *err;            /* 2> file, or 2>> file               */
    int err_append;
    int err_to_out;             /* 2>&1                               */
};

/*
 * The next word of `*s`, NUL-terminated in place, or null if none.
 *
 * With the quoting sh has: '...' is literal, "..." keeps its spaces
 * (its $NAMEs were expanded already, by expand()), and a backslash
 * quotes the next character. The quotes are removed as the word is
 * copied down over itself. *quoted says whether any quoting was seen,
 * so that a quoted ">" is an argument and not a redirection.
 */
static char *next_word(char **s, int *quoted)
{
    char *w, *out;
    char q = 0;

    *quoted = 0;
    while (**s == ' ' || **s == '\t') {
        (*s)++;
    }
    if (**s == '\0') {
        return 0;
    }
    w = out = *s;
    while (**s) {
        char c = **s;

        if (q) {
            if (c == q) {
                q = 0;
            } else if (c == '\\' && q == '"' && (*s)[1]) {
                (*s)++;
                *out++ = **s;
            } else {
                *out++ = c;
            }
        } else if (c == ' ' || c == '\t') {
            break;
        } else if (c == '\'' || c == '"') {
            q = c;
            *quoted = 1;
        } else if (c == '\\' && (*s)[1]) {
            (*s)++;
            *out++ = **s;
            *quoted = 1;
        } else {
            *out++ = c;
        }
        (*s)++;
    }
    /* An unterminated quote runs to the end of the line. */
    if (**s) {
        (*s)++;
    }
    *out = '\0';
    return w;
}

/*
 * Split one command in place into `av`, pulling out its redirections.
 *
 * <, >, >>, 2>, 2>> each take a file name, attached (">out") or as the
 * next word ("> out"); 2>&1 sends stderr wherever stdout goes. All the
 * descriptors are pointed where they go before the command runs, stdout
 * first, so `2>&1` means stdout's destination whatever order they were
 * written in -- simpler than sh, which applies them left to right, and
 * the same in every case anyone writes on purpose.
 *
 * Returns the argument count, -1 for a redirection with no file name,
 * or -2 for more arguments than fit. Truncating instead would silently
 * drop the tail of a command -- `echo` with too many words printed some
 * of them and looked like it had worked, which is a worse failure than
 * refusing.
 */
static int split(char *s, char **av, struct redirs *r, int *background)
{
    int argc = 0;
    char *tok;

    memset(r, 0, sizeof(*r));
    *background = 0;

    int quoted;

    while ((tok = next_word(&s, &quoted)) != 0) {
        const char **target = 0;
        int *append = 0;
        char *rest = 0;

        if (quoted) {
            if (argc == MAX_ARGS) {
                return -2;
            }
            av[argc++] = tok;   /* quoted: an argument, whatever it says */
            continue;
        }

        /*
         * A trailing & asks for the background. Only a whole token:
         * `a&b` is a file name on this system, not two commands.
         */
        if (tok[0] == '&' && tok[1] == '\0') {
            *background = 1;
            continue;
        }
        if (strcmp(tok, "2>&1") == 0) {
            r->err_to_out = 1;
            continue;
        }
        if (tok[0] == '2' && tok[1] == '>') {
            target = &r->err;
            append = &r->err_append;
            rest = tok + 2;
        } else if (tok[0] == '>') {
            target = &r->out;
            append = &r->out_append;
            rest = tok + 1;
        } else if (tok[0] == '<') {
            target = &r->in;
            rest = tok + 1;
        }
        if (target) {
            if (append) {
                *append = 0;
                if (*rest == '>') {
                    *append = 1;
                    rest++;
                }
            }
            if (*rest == '\0') {
                rest = next_word(&s, &quoted);
                if (!rest) {
                    return -1;
                }
            }
            *target = rest;
            continue;
        }

        if (argc == MAX_ARGS) {
            return -2;
        }
        av[argc++] = tok;
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
        "clear                clear the screen (ctrl-L does it too)\n"
        "test / [ ... ]       ask a question; the exit status answers\n"
        "source FILE          run a file of commands\n"
        "set                  the environment\n"
        "export NAME=VALUE    put something in it\n"
        "unset NAME...        take it out\n"
        "ls [-l]              list the directory\n"
        "cd [DIR]             change directory\n"
        "pwd                  where you are\n"
        "mkdir DIR...         make directories\n"
        "rmdir DIR...         remove empty ones\n"
        "cat [FILE...]        print files, or the terminal until ctrl-D\n"
        "hd FILE              hex dump, first 256 bytes\n"
        "cp SRC DST           copy a file\n"
        "mv SRC DST           rename a file\n"
        "rm FILE...           remove files\n"
        "stat FILE            size, mode and modification time\n"
        "df                   space used and available\n"
        "free                 physical memory, in pages\n"

        "echo TEXT            print a line\n"
        "date                 show the date and time\n"
        "date -s DATE [TIME]  set them: YYYY-MM-DD and HH:MM[:SS]\n"
        "uname [-a]           system name, or name and version\n"
        "uptime               how long the machine has been up\n"
        "console [DEV on|off] show or change where console output goes\n"
        "sync                 flush pending writes to the disk\n"
        "ps                   every task on the machine\n"
        "kill [-SIG] PID      send a signal\n"
        "jobs                 this shell's jobs\n"
        "fg [%N]              run a stopped or queued job\n"
        "bg [%N]              run one in the background (see below)\n"
        "history              the lines remembered so far\n"
        "halt                 stop the processor\n"
        "\n"
        "\nThe network tools are PROGRAMS, not builtins: ifconfig, ping\n"
        "and netstat live in /bin and run unprivileged like anything\n"
        "else. `ifconfig dhcp` asks the network for an address.\n"
        "\n"
        "Anything else is looked up as a program on the disk and run --\n"
        "`cube` runs CUBE. Programs have no extension: the kernel decides\n"
        "what is executable by looking at the file, not at its name.\n"
        "`shutdown` is a program, and stops the machine rather than just\n"
        "the processor.\n"
        "\n"
        "> FILE and >> FILE redirect output, so `cat > notes.txt` writes\n"
        "a file and `echo more >> notes.txt` adds to it. < FILE is input,\n"
        "2> FILE is errors, and 2>&1 sends errors where output goes.\n"
        "a | b | c is a pipeline; a builtin may be only its first command.\n"
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
        "  CMD &              run it in the background\n"
        "  jobs               list them;  fg / bg  move one\n"
        "  ps                 every task;  kill [-SIG] PID  signals one\n");
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

/*
 * ls, optionally somewhere else.
 *
 * `getdents` reads the CURRENT directory and takes no path, so listing
 * another one means going there and coming back. That is not elegant,
 * and the alternative -- a path argument on the system call -- is a
 * change to the ABI for the benefit of one caller.
 *
 * It is worth the ugliness because the previous behaviour was to accept
 * `ls /bin`, ignore the argument, and list the current directory
 * instead. Silently answering a different question than the one asked is
 * the worst failure mode available: nothing looks wrong.
 */
static void cmd_ls(int argc, char **args)
{
    struct dirent de;
    int argi = 1;
    int long_form = 0;
    const char *where = 0;
    char saved[PATH_MAX];
    int moved = 0;
    int i = 0;
    int col = 0;
    u32 bytes = 0;

    for (; argi < argc; argi++) {
        if (strcmp(args[argi], "-l") == 0) {
            long_form = 1;
        } else if (!where) {
            where = args[argi];
        } else {
            out_puts("ls: one directory at a time\n");
            return;
        }
    }

    if (where) {
        if (sys_getcwd(saved, sizeof(saved)) < 0) {
            out_puts("ls: cannot find the current directory\n");
            return;
        }
        if (sys_chdir(where) < 0) {
            out_puts("ls: ");
            out_puts(where);
            out_puts(": no such directory\n");
            return;
        }
        moved = 1;
    }

    while (sys_getdents(i, &de) == 0) {
        int n;

        if (long_form) {
            /* The name last, as Unix has it: with long names there is
             * no width a column of them can be padded to. */
            print_mode(de.d_mode);
            out_putdec_pad(de.d_size, 10);
            out_puts("  ");
            print_stamp(de.d_mtime);
            out_puts("  ");
            out_puts(de.d_name);
            out_putc('\n');
        } else {
            /* Five columns of fourteen; a longer name takes a line of
             * its own rather than pushing the grid out of shape. */
            n = (int)strlen(de.d_name);
            if (n >= 14 && col != 0) {
                out_putc('\n');
                col = 0;
            }
            out_puts(de.d_name);
            if (n >= 14) {
                out_putc('\n');
                col = 0;
            } else {
                for (; n < 14; n++) {
                    out_putc(' ');
                }
                if (++col == 5) {
                    out_putc('\n');
                    col = 0;
                }
            }
        }
        bytes += de.d_size;
        i++;
    }

    if (moved) {
        sys_chdir(saved);
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
        out_puts(u.nodename);
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
 * All of it works now. ctrl-Z stops a program and `fg` resumes it from
 * the system call it was in; `&` and `bg` really do run a job while the
 * shell carries on prompting, because there is a scheduler to run them.
 *
 * This comment used to say the opposite -- that the vocabulary promised
 * more than the machine could do, so `&` recorded a job and left it
 * ready for `fg`. That was the honest thing to do at the time and the
 * commands said so rather than pretending.
 *
 * What makes the background safe is that the terminal has ONE
 * foreground pid: a background job that reads gets nothing rather than
 * stealing the keystrokes meant for whoever is at the prompt.
 */
static const char *state_word(int state)
{
    switch (state) {
    case JOB_S_NEW:     return "queued ";
    case JOB_S_RUNNING: return "running";
    case JOB_S_BLOCKED: return "waiting";
    case JOB_S_STOPPED: return "stopped";
    default:            return "done   ";
    }
}

static const char *ps_state(int s)
{
    switch (s) {
    case JOB_S_RUNNING: return "run ";
    case JOB_S_BLOCKED: return "wait";
    case JOB_S_STOPPED: return "stop";
    case JOB_S_DONE:    return "done";
    default:            return "new ";
    }
}

static void cmd_ps(void)
{
    struct job_info info;
    int i;

    out_puts("  PID  PPID  STATE  COMMAND\n");
    for (i = 0; sys_jobctl(JOBCTL_ALL, i, &info) == 0; i++) {
        out_putdec_pad((u32)info.id, 5);
        out_putdec_pad((u32)info.ppid, 6);
        out_puts("  ");
        out_puts(ps_state(info.state));
        out_puts("   ");
        out_puts(info.cmd);
        out_putc('\n');
    }
}

static void cmd_kill(int argc, char **args)
{
    int pid = 0, sig = SIGTERM, i;
    int err;

    /* `kill -9 PID`, the way everybody types it. */
    if (argc > 2 && args[1][0] == '-') {
        sig = 0;
        for (i = 1; args[1][i] >= '0' && args[1][i] <= '9'; i++) {
            sig = sig * 10 + (args[1][i] - '0');
        }
        args++;
        argc--;
    }
    for (i = 0; args[1][i] >= '0' && args[1][i] <= '9'; i++) {
        pid = pid * 10 + (args[1][i] - '0');
    }
    if (pid <= 0 || sig <= 0) {
        err_usage("kill [-SIG] PID");
        return;
    }
    err = sys_kill(pid, sig);
    if (err < 0) {
        err_report("kill", err);
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
static int wait_for(int pid, const char *what);
static int run_script(const char *path);
static int looks_like_script(const char *path);

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

    out_puts(info.cmd);
    out_putc('\n');
    out_flush();

    status = wait_for(id, info.cmd);
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
    if (err < 0) {
        err_report("bg", err);
        return;
    }
    out_putc('[');
    out_putdec((u32)id);
    out_puts("]  ");
    out_puts(info.cmd);
    out_puts(" &\n");
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

/*
 * REDIRECTION IS DONE TO THE SHELL'S OWN DESCRIPTORS.
 *
 * For the length of one command, descriptors 0, 1 and 2 are pointed at
 * the files, and afterwards put back from saved copies -- which is what
 * any shell does for a builtin. A program then needs nothing special at
 * all: spawn gives it the shell's descriptors, so it simply has them.
 *
 * This replaced a private output descriptor the shell wrote its own
 * output through. A program never saw that, so `hello > file` printed
 * on the screen and left an empty file -- redirection that looked as if
 * it had worked.
 */
static int saved_std[3] = { -1, -1, -1 };

/* Point standard descriptor `fd` at whatever `target` refers to. */
static int std_to(int fd, int target)
{
    if (saved_std[fd] < 0) {
        saved_std[fd] = sys_dup(fd);
        if (saved_std[fd] < 0) {
            return saved_std[fd];
        }
        /* The copy must not be given to a program: it would hold the
         * terminal -- or, worse, a pipe's write end -- open behind it. */
        sys_fcntl(saved_std[fd], F_SETFD, FD_CLOEXEC);
    }
    return sys_dup2(target, fd);
}

/* Open `name` and put it on standard descriptor `fd`. */
static int std_open(int fd, const char *name, int flags)
{
    int f = sys_open(name, flags);

    if (f < 0) {
        err_report(name, f);
        return f;
    }
    if (f != fd) {
        int err = std_to(fd, f);

        sys_close(f);
        if (err < 0) {
            err_report(name, err);
            return err;
        }
    }
    return 0;
}

static int redirect(const struct redirs *r)
{
    int w = O_WRONLY | O_CREAT;

    out_flush();
    outlen = 0;
    out_error = 0;
    if (r->in && std_open(STDIN_FILENO, r->in, O_RDONLY) < 0) {
        return -1;
    }
    if (r->out && std_open(STDOUT_FILENO, r->out,
                           w | (r->out_append ? O_APPEND : O_TRUNC)) < 0) {
        return -1;
    }
    if (r->err && std_open(STDERR_FILENO, r->err,
                           w | (r->err_append ? O_APPEND : O_TRUNC)) < 0) {
        return -1;
    }
    if (r->err_to_out && std_to(STDERR_FILENO, STDOUT_FILENO) < 0) {
        return -1;
    }
    return 0;
}

/* Put 0, 1 and 2 back as they were. Safe to call when nothing moved. */
static void redirect_end(void)
{
    int fd, failed;

    out_flush();
    failed = out_error;
    out_error = 0;
    for (fd = 0; fd < 3; fd++) {
        if (saved_std[fd] >= 0) {
            sys_dup2(saved_std[fd], fd);
            sys_close(saved_std[fd]);
            saved_std[fd] = -1;
        }
    }
    if (failed) {
        err_puts("write failed\n");
    }
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
    if (status > 128 && status < 128 + NSIG) {
        err_puts(": ");
        err_puts(strsignal(status - 128));
        err_puts("\n");
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
 * Find a command and start it.
 *
 * A name with a slash in it is a path and is used as given, the way
 * every shell does it -- that is what makes ./prog mean "this one, here"
 * rather than "search for it". Anything else is looked for in each
 * directory of PATH in turn, and with no PATH set, in the current
 * directory only.
 *
 * The search stops at the first directory that has it, which is what
 * makes PATH an order of preference rather than a set.
 */
static int spawn_on_path(const char *name, int argc, char **args)
{
    const char *path = env_get("PATH");
    char full[PATH_MAX];
    int has_slash = 0, i;

    for (i = 0; name[i]; i++) {
        if (name[i] == '/') {
            has_slash = 1;
        }
    }
    if (has_slash || !path || !*path) {
        return sys_spawn(name, argc, args, env);
    }

    while (*path) {
        u32 n = 0;

        while (*path && *path != ':' && n + 2 < sizeof(full)) {
            full[n++] = *path++;
        }
        if (n > 0 && full[n - 1] != '/') {
            full[n++] = '/';
        }
        {
            const char *p = name;

            while (*p && n + 1 < sizeof(full)) {
                full[n++] = *p++;
            }
        }
        full[n] = '\0';

        {
            int r = sys_spawn(full, argc, args, env);

            /*
             * Only "not there" moves on to the next directory. A
             * program that exists and could not be run is an error to
             * report, not a reason to go looking for a different
             * program of the same name.
             */
            if (r != -ENOENT && r != -EINVAL) {
                return r;
            }
        }

        while (*path == ':') {
            path++;
        }
    }
    return -ENOENT;
}

/*
 * Wait for a foreground job, holding the terminal while it runs.
 *
 * The terminal's foreground task is what ctrl-C and ctrl-Z are aimed at,
 * so it moves to the child for as long as the shell is waiting and comes
 * back afterwards. Without that the shell would be signalling itself.
 *
 * Returns the child's exit status, or SPAWN_STOPPED if it stopped rather
 * than finished -- in which case it is still there, and `fg` resumes it.
 */
static int wait_for(int pid, const char *what)
{
    int status = 0;
    int got;

    (void)what;
    sys_jobctl(JOBCTL_FG, pid, 0);

    got = sys_waitpid(pid, &status, WUNTRACED);
    sys_jobctl(JOBCTL_FG, 0, 0);        /* the terminal comes back */

    if (got == pid && WIFSTOPPED(status)) {
        /* ctrl-Z. It is still there, and `fg` resumes it. */
        out_putc('\n');
        out_putc('[');
        out_putdec((u32)pid);
        out_puts("]+  stopped\n");
        out_flush();
        return SPAWN_STOPPED;
    }

    if (got < 0) {
        /*
         * No child to wait for. The usual reason is that it stopped
         * rather than exited -- a stopped task is not a zombie -- so
         * look and see.
         */
        struct job_info info;
        int i;

        for (i = 0; sys_jobctl(JOBCTL_INFO, i, &info) == 0; i++) {
            if (info.id == pid && info.state == JOB_S_STOPPED) {
                return SPAWN_STOPPED;
            }
        }
        return got;
    }
    /*
     * The kernel reports Linux's status word; the shell speaks the
     * convention every shell reports and `$?` shows: the exit code, or
     * 128 plus the signal that ended it.
     */
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return WEXITSTATUS(status);
}

/*
 * Run one command line.
 *
 * Separate from the read loop because `fg` on a job that was queued with
 * & has a command line and no context -- it has to be able to run one
 * without there being a prompt involved.
 */
/*
 * Replace $NAME with its value, in place.
 *
 * In place because the line buffer is already the right size and a
 * second one would only exist to be copied back. Expansion happens
 * BEFORE splitting, which is what makes a variable able to contain more
 * than one word -- and is also why a value with a space in it is split,
 * exactly as an unquoted expansion is in any shell.
 *
 * ${NAME} is accepted so that a variable can be followed immediately by
 * a letter. $$ is this shell's pid, because a script needs some way to
 * make a name nothing else will use.
 */
static int expand(char *line, u32 max)
{
    char out[2 * LINE_MAX];     /* room to notice going past max */
    u32 i = 0, o = 0;

    int in_single = 0, in_double = 0;

    while (line[i] && o + 1 < sizeof(out)) {
        char name[32];
        u32 n = 0;
        const char *val;
        int braced = 0;

        /*
         * Quotes are left in place for split() to remove, but they
         * decide what is expanded here: nothing inside '...', and a
         * backslash protects the next character anywhere but there.
         */
        if (line[i] == '\'' && !in_double) {
            in_single = !in_single;
        } else if (line[i] == '"' && !in_single) {
            in_double = !in_double;
        } else if (line[i] == '\\' && !in_single && line[i + 1]) {
            out[o++] = line[i++];
            if (o + 1 < sizeof(out)) {
                out[o++] = line[i++];
            }
            continue;
        }
        if (line[i] != '$' || !line[i + 1] || in_single) {
            out[o++] = line[i++];
            continue;
        }
        i++;

        if (line[i] == '$') {
            char buf[12];
            int k = 0, pid = sys_getpid();

            i++;
            if (!pid) {
                buf[k++] = '0';
            }
            while (pid > 0) {
                buf[k++] = (char)('0' + pid % 10);
                pid /= 10;
            }
            while (k > 0 && o + 1 < sizeof(out)) {
                out[o++] = buf[--k];
            }
            continue;
        }
        if (line[i] == '?') {
            /* The last command's status, which is what a script tests. */
            char buf[12];
            int k = 0, v = last_status;

            i++;
            if (!v) {
                buf[k++] = '0';
            }
            while (v > 0) {
                buf[k++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (k > 0 && o + 1 < sizeof(out)) {
                out[o++] = buf[--k];
            }
            continue;
        }
        if (line[i] == '{') {
            braced = 1;
            i++;
        }
        while (line[i] && n + 1 < sizeof(name) &&
               ((line[i] >= 'A' && line[i] <= 'Z') ||
                (line[i] >= 'a' && line[i] <= 'z') ||
                (line[i] >= '0' && line[i] <= '9') || line[i] == '_')) {
            name[n++] = line[i++];
        }
        name[n] = '\0';
        if (braced && line[i] == '}') {
            i++;
        }
        if (!n) {
            out[o++] = '$';     /* a lone $ is just a dollar sign */
            continue;
        }
        val = env_get(name);
        while (val && *val && o + 1 < sizeof(out)) {
            out[o++] = *val++;
        }
    }
    out[o] = '\0';
    /* Too long once expanded: an error, never a command cut short. */
    if (line[i] || o >= max) {
        return -1;
    }
    memcpy(line, out, o + 1);
    return 0;
}

/*
 * Run argv[0] if it is a builtin. Returns 1 if it was, 0 if it is not
 * one and should be looked for as a program.
 *
 * Its own function, where it used to be the body of run_command, so that
 * a builtin can be one stage of a pipeline: the first stage of `ls |
 * prog` is the shell itself, writing into the pipe.
 */
static int run_builtin(int argc)
{
    int err;
    int i;

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

    } else if (strcmp(argv[0], "free") == 0) {
        struct sysinfo si;

        err = sys_sysinfo(&si);
        if (err < 0) {
            err_report("free", err);
        } else {
            u32 kb = si.mem_unit / 1024;

            out_puts("           pages       KB\n");
            out_puts("total  ");
            out_putdec_pad(si.totalram, 10);
            out_putdec_pad(si.totalram * kb, 9);
            out_putc('\n');
            /* "used" leaves out the cache, as Linux's free does: those
             * pages are held only in case somebody maps the file again,
             * and are given up the moment memory is wanted. */
            out_puts("used   ");
            out_putdec_pad(si.totalram - si.freeram - si.bufferram, 10);
            out_putdec_pad((si.totalram - si.freeram - si.bufferram) * kb, 9);
            out_putc('\n');
            out_puts("cache  ");
            out_putdec_pad(si.bufferram, 10);
            out_putdec_pad(si.bufferram * kb, 9);
            out_putc('\n');
            out_puts("free   ");
            out_putdec_pad(si.freeram, 10);
            out_putdec_pad(si.freeram * kb, 9);
            out_putc('\n');
            if (si.totalswap) {
                out_puts("swap   ");
                out_putdec_pad(si.totalswap, 10);
                out_putdec_pad(si.totalswap * kb, 9);
                out_puts("   ");
                out_putdec(si.totalswap - si.freeswap);
                out_puts(" pages in use\n");
            }
            out_puts("\npage size ");
            out_putdec(si.mem_unit);
            out_puts(" bytes, ");
            out_putdec(si.procs);
            out_puts(" job(s)\n");
        }

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

    } else if (strcmp(argv[0], "clear") == 0) {
        /*
         * Home, then erase. Both sinks understand it: the serial
         * terminal because it is a terminal, and the framebuffer
         * console because it is one too, a VT102.
         */
        out_puts("\033[H\033[2J");

    } else if (strcmp(argv[0], "test") == 0 ||
               strcmp(argv[0], "[") == 0) {
        /*
         * Enough of test(1) for a script to ask whether something
         * is there. The exit status IS the answer, which is what
         * makes `if test -f X` work without any other machinery.
         */
        int r = 1;

        if (strcmp(argv[argc - 1], "]") == 0) {
            argc--;                 /* the closing bracket */
        }
        if (argc == 2) {
            r = argv[1][0] ? 0 : 1;             /* test STRING */
        } else if (argc == 3 && argv[1][0] == '-') {
            struct stat st;

            switch (argv[1][1]) {
            case 'f':
                r = (sys_stat(argv[2], &st) == 0 &&
                     !(st.st_mode & S_IFDIR)) ? 0 : 1;
                break;
            case 'd':
                r = (sys_stat(argv[2], &st) == 0 &&
                     (st.st_mode & S_IFDIR)) ? 0 : 1;
                break;
            case 'e':
                r = sys_stat(argv[2], &st) == 0 ? 0 : 1;
                break;
            case 'n':
                r = argv[2][0] ? 0 : 1;
                break;
            case 'z':
                r = argv[2][0] ? 1 : 0;
                break;
            default:
                err_puts("test: unknown test\n");
                break;
            }
        } else if (argc == 4 && strcmp(argv[2], "=") == 0) {
            r = strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
        } else if (argc == 4 && strcmp(argv[2], "!=") == 0) {
            r = strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
        }
        last_status = r;
        status_set = 1;
        return 1;

    } else if (strcmp(argv[0], "exit") == 0) {
        /*
         * /bin/sh ends, with the status given or the last one. The
         * machine's own shell has nothing to return to; `shutdown` is
         * how the machine stops.
         */
        if (!shell_is_program) {
            err_puts("exit: this is the machine's own shell; "
                     "`shutdown` stops the machine\n");
        } else {
            int code = last_status;

            if (argc > 1) {
                const char *p = argv[1];

                code = 0;
                while (*p >= '0' && *p <= '9') {
                    code = code * 10 + (*p++ - '0');
                }
            }
            redirect_end();
            out_flush();
            sys_exit(code & 0xff);
        }

    } else if (strcmp(argv[0], "source") == 0 ||
               strcmp(argv[0], ".") == 0) {
        if (need(argc, 2, "source FILE")) {
            err = run_script(argv[1]);
            if (err < 0) {
                err_report(argv[1], err);
            }
        }

    } else if (strcmp(argv[0], "set") == 0) {
        /*
         * `set` is the builtin and `env` deliberately is NOT: env is
         * a program, and running it proves the environment actually
         * crossed into another address space rather than merely
         * existing in this one.
         */
        for (i = 0; i < env_count; i++) {
            out_puts(env_store[i]);
            out_putc('\n');
        }

    } else if (strcmp(argv[0], "export") == 0) {
        /*
         * `export NAME=VALUE`, and `export NAME` to clear it. There
         * is no distinction between a shell variable and an
         * exported one here: everything the shell holds is the
         * environment, because with no shell functions there is
         * nothing a private variable would be private from.
         */
        if (need(argc, 2, "export NAME=VALUE")) {
            for (i = 1; i < argc; i++) {
                char *eq = strchr(argv[i], '=');

                if (eq) {
                    *eq = '\0';
                    err = env_set(argv[i], eq + 1);
                    *eq = '=';
                } else {
                    err = env_set(argv[i], "");
                }
                if (err < 0) {
                    err_report(argv[i], err);
                }
            }
        }

    } else if (strcmp(argv[0], "unset") == 0) {
        if (need(argc, 2, "unset NAME...")) {
            for (i = 1; i < argc; i++) {
                env_unset(argv[i]);
            }
        }

    } else if (strcmp(argv[0], "cd") == 0) {
        /* Bare `cd` goes to the root, which is this machine's home
         * directory -- there is no user to have one of their own. */
        err = sys_chdir(argc > 1 ? argv[1] : "/");
        if (err < 0) {
            err_report(argc > 1 ? argv[1] : "/", err);
        }

    } else if (strcmp(argv[0], "pwd") == 0) {
        char cwd[PATH_MAX];

        if (sys_getcwd(cwd, sizeof(cwd)) < 0) {
            err_puts("pwd: cannot tell\n");
        } else {
            out_puts(cwd);
            out_putc('\n');
        }

    } else if (strcmp(argv[0], "mkdir") == 0) {
        if (need(argc, 2, "mkdir DIR...")) {
            for (i = 1; i < argc; i++) {
                err = sys_mkdir(argv[i]);
                if (err < 0) {
                    err_report(argv[i], err);
                }
            }
        }

    } else if (strcmp(argv[0], "rmdir") == 0) {
        if (need(argc, 2, "rmdir DIR...")) {
            for (i = 1; i < argc; i++) {
                err = sys_rmdir(argv[i]);
                if (err < 0) {
                    err_report(argv[i], err);
                }
            }
        }

    } else if (strcmp(argv[0], "ps") == 0) {
        cmd_ps();

    } else if (strcmp(argv[0], "kill") == 0) {
        if (need(argc, 2, "kill [-SIG] PID")) {
            cmd_kill(argc, argv);
        }

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
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* Pipelines                                                         */
/* ---------------------------------------------------------------- */

#define MAX_STAGES  8

/* Builtins read the global argv; a pipeline stage's arguments go there. */
static void argv_from(char **av, int ac)
{
    int i;

    for (i = 0; i < ac; i++) {
        argv[i] = av[i];
    }
}

static const char *strchr_(const char *s, char c)
{
    for (; *s; s++) {
        if (*s == c) {
            return s;
        }
    }
    return 0;
}

/* Is there a | outside quotes -- is this a pipeline? */
static int has_bar(const char *p)
{
    char q = 0;

    for (; *p; p++) {
        if (q) {
            if (*p == q) {
                q = 0;
            } else if (*p == '\\' && q == '"' && p[1]) {
                p++;
            }
        } else if (*p == '\'' || *p == '"') {
            q = *p;
        } else if (*p == '\\' && p[1]) {
            p++;
        } else if (*p == '|') {
            return 1;
        }
    }
    return 0;
}

/*
 * a | b | c
 *
 * Every pipe is made first, close-on-exec, so no program is handed an
 * end it was not meant to have -- a stray copy of a write end held by
 * the wrong program is a reader that never sees end of file. Then each
 * program is started with its ends moved onto 0 and 1 (dup2 clears
 * close-on-exec on the copy), all of them in the first one's process
 * group, which gets the terminal: ctrl-C reaches every stage.
 *
 * A BUILTIN may be the first stage, and nowhere else. It is the shell
 * itself writing into the pipe, so it runs after the programs that read
 * from it have started; a builtin reading from a pipe would need the
 * shell to be two things at once. The pipeline's status is the last
 * stage's, as in sh.
 */
static void run_pipeline(char *line)
{
    char *stage_text[MAX_STAGES];
    char *av[MAX_STAGES][MAX_ARGS];
    struct redirs r[MAX_STAGES];
    int ac[MAX_STAGES], pid[MAX_STAGES];
    int rd[MAX_STAGES], wr[MAX_STAGES];
    int n = 0, i, background = 0, leader = 0, status = 0, stopped = 0;
    int missing_last = 0;
    char *p = line;

    /* Cut it at the bars -- not the ones inside quotes. */
    char q = 0;

    stage_text[n++] = p;
    for (; *p; p++) {
        if (q) {
            if (*p == q) {
                q = 0;
            } else if (*p == '\\' && q == '"' && p[1]) {
                p++;
            }
            continue;
        }
        if (*p == '\'' || *p == '"') {
            q = *p;
            continue;
        }
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '|') {
            *p = '\0';
            if (n == MAX_STAGES) {
                err_puts("too many commands in one pipeline\n");
                return;
            }
            stage_text[n++] = p + 1;
        }
    }
    for (i = 0; i < n; i++) {
        int bg;

        ac[i] = split(stage_text[i], av[i], &r[i], &bg);
        if (ac[i] == -1) {
            err_puts("syntax error: a redirection needs a file name\n");
            return;
        }
        if (ac[i] == -2) {
            err_puts("too many arguments\n");
            return;
        }
        if (ac[i] == 0) {
            err_puts("syntax error: an empty command in a pipeline\n");
            return;
        }
        if (bg && i != n - 1) {
            err_puts("syntax error: & belongs at the end\n");
            return;
        }
        background = bg;
        pid[i] = 0;
        rd[i] = wr[i] = -1;
    }

    for (i = 0; i < n - 1; i++) {
        int fds[2];

        if (sys_pipe(fds) < 0) {
            err_puts("pipe: cannot make one\n");
            goto close_pipes;
        }
        rd[i] = fds[0];
        wr[i] = fds[1];
        sys_fcntl(rd[i], F_SETFD, FD_CLOEXEC);
        sys_fcntl(wr[i], F_SETFD, FD_CLOEXEC);
    }

    /*
     * The programs, last first, so that each has its reader running
     * before anything can write to it. Stage 0 is tried as a builtin
     * after these; only if it is not one is it started as a program.
     */
    for (i = n - 1; i >= 0; i--) {
        int got;

        if (i > 0 && std_to(STDIN_FILENO, rd[i - 1]) < 0) {
            redirect_end();
            goto close_pipes;
        }
        if (i < n - 1 && std_to(STDOUT_FILENO, wr[i]) < 0) {
            redirect_end();
            goto close_pipes;
        }
        /* A stage's own redirections win over the pipe, as in sh. */
        if (redirect(&r[i]) != 0) {
            redirect_end();
            if (i == n - 1) {
                status = 1;             /* as for a single command */
                missing_last = 1;
            }
            goto close_pipes;
        }
        if (i == 0) {
            argv_from(av[0], ac[0]);
            if (run_builtin(ac[0])) {
                redirect_end();
                break;
            }
        }
        got = spawn_on_path(av[i][0], ac[i], av[i]);
        redirect_end();
        if (got < 0) {
            err_puts(av[i][0]);
            err_puts(got == -ENOENT || got == -EINVAL ?
                     ": command not found\n" : ": cannot run\n");
            if (i == n - 1) {
                status = got == -ENOENT || got == -EINVAL ? 127 : 126;
                missing_last = 1;
            }
            continue;
        }
        pid[i] = got;
        /* The group is led by the first program started -- the last
         * stage -- and every other stage joins it. */
        if (!leader) {
            leader = got;
        }
        sys_setpgid(got, leader);
    }

close_pipes:
    /* The shell's own ends, so that the readers see end of file when
     * the last WRITER among the stages finishes. */
    for (i = 0; i < n - 1; i++) {
        if (rd[i] >= 0) {
            sys_close(rd[i]);
        }
        if (wr[i] >= 0) {
            sys_close(wr[i]);
        }
    }
    if (!leader) {
        if (missing_last) {
            last_status = status;
        }
        return;
    }

    if (background) {
        sys_jobctl(JOBCTL_BG, leader, 0);
        out_putc('[');
        out_putdec((u32)leader);
        out_puts("]  ");
        out_puts(cmdline_saved);
        out_putc('\n');
        return;
    }

    /* The whole group has the terminal while the shell waits for each. */
    for (i = 0; i < n; i++) {
        if (pid[i] > 0) {
            int st = wait_for(pid[i], av[i][0]);

            if (st == SPAWN_STOPPED) {
                stopped = 1;
            } else if (i == n - 1 && !missing_last) {
                status = st;
            }
        }
    }
    if (!stopped) {
        last_status = status;
        report_status(av[n - 1][0], status);
    }
    sys_jobctl(JOBCTL_REAP, 0, 0);
}

/*
 * A COMMAND LIST: commands joined by ";" (one after the other), "&&"
 * (the next only if this one succeeded), "||" (only if it failed), and
 * a lone "&" (this one in the background, and on to the next). && and
 * || bind equally and left to right, as in sh, so "a && b || c" runs c
 * if either a or b failed -- which is what the status of the last
 * command run says, with no nesting needed.
 *
 * Nothing inside quotes, or after a backslash, separates. A lone & is
 * a separator only as a whole word with more after it: "2>&1" is a
 * redirection, "a&b" is a file name here, and a trailing & belongs to
 * its command, as it always did.
 *
 * Each command is expanded when it runs, not before, so "false; echo $?"
 * sees the status of false.
 */

static int more_after(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return *p != '\0';
}

static void run_list(char *line)
{
    char seg[LINE_MAX];
    const char *p = line, *start = line;
    char q = 0;
    int cond = 0;               /* 0 always, 1 after success, 2 after failure */

    for (;;) {
        char c = *p;
        int end = 0, next = 0, amp = 0, adv = 1;

        if (c == '\0') {
            end = 1;
            adv = 0;
        } else if (q) {
            if (c == q) {
                q = 0;
            } else if (c == '\\' && q == '"' && p[1]) {
                p++;
            }
        } else if (c == '\'' || c == '"') {
            q = c;
        } else if (c == '\\' && p[1]) {
            p++;
        } else if (c == ';') {
            end = 1;
        } else if (c == '&' && p[1] == '&') {
            end = 1;
            next = 1;
            adv = 2;
        } else if (c == '|' && p[1] == '|') {
            end = 1;
            next = 2;
            adv = 2;
        } else if (c == '&' && (p == line || p[-1] == ' ' || p[-1] == '\t') &&
                   (p[1] == ' ' || p[1] == '\t') && more_after(p + 1)) {
            end = 1;
            amp = 1;
        }

        if (end) {
            u32 n = (u32)(p - start);
            u32 a = 0, b;

            if (n > sizeof(seg) - 3) {
                /* Refused, never cut short. */
                err_puts("sh: command too long\n");
                last_status = 2;
                n = 0;
            }
            memcpy(seg, start, n);
            b = n;
            if (amp) {
                seg[b++] = ' ';
                seg[b++] = '&';
            }
            seg[b] = '\0';
            while (seg[a] == ' ' || seg[a] == '\t') {
                a++;
            }
            if (seg[a] && (cond == 0 || (cond == 1 && last_status == 0) ||
                           (cond == 2 && last_status != 0))) {
                strncpy(cmdline_saved, seg + a, sizeof(cmdline_saved) - 1);
                cmdline_saved[sizeof(cmdline_saved) - 1] = '\0';
                if (opt_xtrace) {
                    err_puts("+ ");
                    err_puts(seg + a);
                    err_puts("\n");
                }
                run_command(seg + a);
                /* -e: a failure that is not the left of && or || ends
                 * the shell, with that status. */
                if (opt_errexit && last_status != 0 && next == 0) {
                    out_flush();
                    sys_exit(last_status);
                }
            }
            cond = next;
            if (c == '\0') {
                break;
            }
            p += adv;
            start = p;
            continue;
        }
        p++;
    }
}

static void run_command(char *cmdline)
{
    struct redirs redir;
    int background;
    int argc;
    int err;

    {
        if (expand(cmdline, LINE_MAX) < 0) {
            err_puts("sh: command too long\n");
            last_status = 2;
            return;
        }
        if (has_bar(cmdline)) {
            run_pipeline(cmdline);
            return;
        }
        argc = split(cmdline, argv, &redir, &background);
        if (argc == -1) {
            err_puts("syntax error: a redirection needs a file name\n");
            return;
        }
        if (argc == -2) {
            err_puts("too many arguments\n");
            return;
        }
        if (argc == 0) {
            return;
        }

        /* A redirection that cannot be made means the command is not
         * run, and its status is 1, as in every POSIX shell -- it was
         * left at whatever the last command set. */
        if (redirect(&redir) != 0) {
            redirect_end();
            last_status = 1;
            return;
        }

        builtin_failed = 0;
        status_set = 0;
        if (run_builtin(argc)) {
            if (!status_set) {
                last_status = builtin_failed ? 1 : 0;
            }
        } else {
            /*
             * Not a builtin, so look for a program of that name --
             * along $PATH, in spawn_on_path() above, unless the name
             * has a slash in it and is therefore already a path.
             */
            int status;

            out_flush();

            /*
             * spawn STARTS it and returns its pid. Whether to wait is
             * the shell's decision, and that decision is the whole of
             * what & means: a foreground job is one the shell waits
             * for, and a background job is one it does not.
             */
            status = spawn_on_path(argv[0], argc, argv);

            if (status > 0) {
                int pid = status;

                /*
                 * A job is a process group of its own, as in any shell
                 * with job control. That is what lets the terminal be
                 * handed to it -- ctrl-C reaches it and anything it
                 * starts -- and what keeps the key away from jobs in the
                 * background.
                 */
                sys_setpgid(pid, pid);

                /* The program has its own copies of the descriptors
                 * now; the shell's go back to the terminal, where its
                 * own messages about the job belong. */
                redirect_end();

                if (background) {
                    sys_jobctl(JOBCTL_BG, pid, 0);
                    out_putc('[');
                    out_putdec((u32)pid);
                    out_puts("]  ");
                    out_puts(cmdline_saved);
                    out_putc('\n');
                    return;
                }
                status = wait_for(pid, argv[0]);
            }

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
                last_status = 127;          /* what every sh says */
            } else if (status == -ENOEXEC) {
                /*
                 * Not an ELF image. It may be a script -- but only if
                 * it says so by starting with '#'.
                 *
                 * There is no execute permission on a FAT volume and no
                 * way to add one, so something has to distinguish a
                 * script from a data file that happens to share its
                 * name. Unix uses the exec bit and the #! line; with
                 * only the second available, a leading '#' is the whole
                 * test. Running any text file instead would mean a
                 * mistyped `cat` executing somebody's notes.
                 */
                if (looks_like_script(argv[0])) {
                    err = run_script(argv[0]);
                    if (err < 0) {
                        err_report(argv[0], err);
                    }
                } else {
                    err_puts(argv[0]);
                    err_puts(": not an executable\n");
                    last_status = 126;
                }
            } else if (status < 0) {
                err_report(argv[0], status);
                last_status = 126;
            } else {
                last_status = status;
                report_status(argv[0], status);
                sys_jobctl(JOBCTL_REAP, 0, 0);
            }
        }
        redirect_end();
    }
}

/* ---------------------------------------------------------------- */
/* Scripts                                                           */
/* ---------------------------------------------------------------- */

/*
 * A script is a file of commands, and the only things it needs beyond
 * that are comments and a way to ask a question.
 *
 * THE FILE IS READ A BUFFER AT A TIME, on a descriptor of the shell's own
 * that is close-on-exec -- so a program the script starts can neither
 * read the rest of the script nor move the shell's place in it. It used
 * to be read whole into 4 KB and anything after that was silently not
 * run; a script is now as long as the file, and nesting (source) is
 * safe because each level has its own reader.
 *
 * The conditional is line-structured -- `if`, `then`, `else`, `fi` each
 * on their own line -- because that needs a stack of flags rather than a
 * parser, and because the thing a startup script actually needs to say
 * is "if this exists, do that".
 */
#define IF_DEPTH      8

struct script_reader {
    int  fd;
    int  pos, len;
    char buf[256];
};

/*
 * The next line into `out`, without its newline. Returns its length, or
 * -1 at the end of the file. A line longer than `max` - 1 is consumed
 * whole and reported with *too_long, never run cut short.
 */
static int script_getline(struct script_reader *r, char *out, u32 max,
                          int *too_long)
{
    u32 n = 0;
    int any = 0;

    *too_long = 0;
    for (;;) {
        char c;

        if (r->pos >= r->len) {
            s32 got = sys_read(r->fd, r->buf, sizeof(r->buf));

            if (got <= 0) {
                break;
            }
            r->pos = 0;
            r->len = (int)got;
        }
        c = r->buf[r->pos++];
        any = 1;
        if (c == '\n') {
            break;
        }
        if (n + 1 < max) {
            out[n++] = c;
        } else {
            *too_long = 1;
        }
    }
    out[n] = '\0';
    return any ? (int)n : -1;
}

static int script_line_is(const char *line, const char *word)
{
    u32 i = 0;

    while (line[i] == ' ' || line[i] == '\t') {
        i++;
    }
    while (*word && line[i] == *word) {
        i++;
        word++;
    }
    if (*word) {
        return 0;
    }
    while (line[i] == ' ' || line[i] == '\t') {
        i++;
    }
    return line[i] == '\0';
}

/*
 * Does this file claim to be a script?
 *
 * One character decides it. A shebang line is accepted and then treated
 * as a comment, because there is only one shell here for it to name.
 */
static int looks_like_script(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    char c = 0;
    s32 n;

    if (fd < 0) {
        return 0;
    }
    n = sys_read(fd, &c, 1);
    sys_close(fd);
    return n == 1 && c == '#';
}

static int run_script(const char *path)
{
    /*
     * executing[d] says whether this nesting level is running, and
     * taken[d] whether a branch at this level already matched -- which
     * is what stops `else` from running after a true `if`.
     */
    int executing[IF_DEPTH];
    int taken[IF_DEPTH];
    int depth = 0;
    int too_long;
    struct script_reader rd;
    char line[LINE_MAX];

    rd.fd = sys_open(path, O_RDONLY | O_CLOEXEC);
    if (rd.fd < 0) {
        return rd.fd;
    }
    rd.pos = rd.len = 0;

    executing[0] = 1;
    taken[0] = 1;

    while (script_getline(&rd, line, sizeof(line), &too_long) >= 0) {
        if (too_long) {
            err_puts(path);
            err_puts(": a line longer than the shell takes, not run\n");
            last_status = 2;
            continue;
        }

        /* Strip a trailing carriage return, so a script written on the
         * host with DOS line endings works. */
        {
            int L = (int)strlen(line);

            while (L > 0 && (line[L - 1] == '\r' || line[L - 1] == ' ')) {
                line[--L] = '\0';
            }
        }

        {
            u32 k = 0;

            while (line[k] == ' ' || line[k] == '\t') {
                k++;
            }
            if (line[k] == '\0' || line[k] == '#') {
                continue;               /* blank, or a comment */
            }
        }

        if (script_line_is(line, "fi")) {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        if (script_line_is(line, "else")) {
            if (depth > 0) {
                executing[depth] = executing[depth - 1] && !taken[depth];
            }
            continue;
        }
        if (script_line_is(line, "then")) {
            continue;                   /* accepted and ignored */
        }

        {
            u32 k = 0;

            while (line[k] == ' ' || line[k] == '\t') {
                k++;
            }
            if (line[k] == 'i' && line[k + 1] == 'f' &&
                (line[k + 2] == ' ' || line[k + 2] == '\t')) {
                int cond = 0;

                if (depth + 1 >= IF_DEPTH) {
                    err_puts("script: if nested too deeply\n");
                    sys_close(rd.fd);
                    return -E2BIG;
                }
                if (executing[depth]) {
                    char sub[LINE_MAX];

                    strncpy(sub, &line[k + 3], sizeof(sub) - 1);
                    sub[sizeof(sub) - 1] = '\0';
                    /* A trailing "; then" is how everybody writes it. */
                    {
                        int L = (int)strlen(sub);

                        while (L > 4 &&
                               strcmp(&sub[L - 4], "then") == 0) {
                            sub[L - 4] = '\0';
                            L -= 4;
                            while (L > 0 && (sub[L - 1] == ' ' ||
                                             sub[L - 1] == ';')) {
                                sub[--L] = '\0';
                            }
                        }
                    }
                    last_status = 0;
                    run_list(sub);
                    cond = (last_status == 0);
                }
                depth++;
                executing[depth] = executing[depth - 1] && cond;
                taken[depth] = cond;
                continue;
            }
        }

        if (!executing[depth]) {
            continue;
        }

        {
            char work[LINE_MAX];

            strncpy(work, line, sizeof(work) - 1);
            work[sizeof(work) - 1] = '\0';
            strncpy(cmdline_saved, work, sizeof(cmdline_saved) - 1);
            cmdline_saved[sizeof(cmdline_saved) - 1] = '\0';
            run_list(work);
        }
    }

    sys_close(rd.fd);
    return 0;
}

/*
 * The prompt loop.
 *
 * All the editing is in edit.c, above the system call boundary, the way
 * a shell's is. What is left here is what a shell's loop actually is:
 * read a line, remember it, run it. Returns only for /bin/sh, at end of
 * input; the machine's own shell has nowhere to return to.
 */
static void interactive(void)
{
    for (;;) {
        char prompt[PATH_MAX + 8];
        int n;

        /*
         * Reap anything that finished while we were away.
         *
         * A real shell does this on SIGCHLD; there are no handlers here
         * yet, so the prompt is the place. It was only done by `jobs`
         * and by `fg`, which meant a background job that ended -- or
         * was killed -- held its address space, its kernel stack and
         * one of the eight task slots until somebody happened to ask
         * for a listing. With a 256 MB address space that is a
         * megabyte a time, and with eight slots it is a machine that
         * stops being able to start anything after a few.
         */
        sys_jobctl(JOBCTL_REAP, 0, 0);

        /*
         * The prompt carries the directory, because with more than one
         * of them a bare "sage$" stops saying enough.
         */
        {
            char cwd[PATH_MAX];
            u32 k = 0;

            if (sys_getcwd(cwd, sizeof(cwd)) < 0) {
                strcpy(cwd, "?");
            }
            while (cwd[k] && k < sizeof(prompt) - 4) {
                prompt[k] = cwd[k];
                k++;
            }
            prompt[k++] = '$';
            prompt[k++] = ' ';
            prompt[k] = '\0';
        }

        n = edit_readline(prompt, line, (int)sizeof(line));

        if (n == -EINTR) {
            continue;           /* ctrl-C: a fresh prompt, nothing run */
        }
        if (n < 0) {
            /* End of input on the terminal. /bin/sh ends, as any shell
             * does; the machine's own shell has nowhere to exit to, so
             * it starts a fresh line and carries on. */
            out_putc('\n');
            out_flush();
            if (shell_is_program) {
                return;
            }
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
        run_list(line);
    }
}

/* The machine's own shell: a kernel task, started at boot. */
void shell(void)
{
    /*
     * The defaults, before /etc/rc gets a chance to change them.
     *
     * PATH has /bin first because that is where the system's own
     * programs live, and the current directory last -- which is the
     * ordering that stops a program dropped in the working directory
     * from quietly replacing a system one.
     */
    env_set("PATH", "/bin:.");
    env_set("HOME", "/");
    env_set("SHELL", "/bin/sh");
    env_set("TERM", "vt102");   /* what fbcon.c is, and any serial terminal can be */

    /*
     * The clock keeps UTC, as a machine's clock should, and TZ is how a
     * program turns that into a local time. UTC0 is the honest default:
     * a machine that has not been told where it is should not guess.
     *
     * There is no zoneinfo database here and does not need to be -- a
     * POSIX TZ string carries its own rules, which is what the format
     * is for. `export TZ=MST7MDT,M3.2.0,M11.1.0` in /etc/rc is a
     * machine in Colorado, daylight saving included.
     */
    env_set("TZ", "UTC0");

    /*
     * /etc/rc, if there is one. Not an error if there is not: a machine
     * with a blank disk should still come up to a prompt, and saying
     * "no such file" at every boot would be noise rather than news.
     *
     * Note the name. rc.local would be the conventional one and is not
     * a legal 8.3 name -- five characters of extension -- so the
     * filesystem chose this, not taste.
     */
    {
        struct stat st;

        if (sys_stat("/etc/rc", &st) == 0) {
            run_script("/etc/rc");
        }
    }

    interactive();
}

/*
 * /bin/sh: this same shell, built as a program. `sh -c 'COMMAND'` runs
 * one command line, `sh FILE` runs a script, and `sh` alone is
 * interactive until end of input or `exit`. It takes the environment
 * it was given rather than setting defaults, and does not run /etc/rc,
 * which belongs to the machine and not to every shell.
 */
int shell_main(int argc, char **args, char **envp)
{
    int i;

    shell_is_program = 1;
    for (i = 0; envp && envp[i]; i++) {
        char name[64];
        const char *eq = strchr_(envp[i], '=');
        u32 n;

        if (!eq) {
            continue;
        }
        n = (u32)(eq - envp[i]);
        if (n >= sizeof(name)) {
            continue;
        }
        memcpy(name, envp[i], n);
        name[n] = '\0';
        env_set(name, eq + 1);
    }
    if (!env_get("PATH")) {
        env_set("PATH", "/bin:.");
    }
    if (!env_get("SHELL")) {
        env_set("SHELL", "/bin/sh");
    }
    if (!env_get("TERM")) {
        env_set("TERM", "vt102");
    }

    /*
     * Options, in clusters as POSIX has them: -c (the command is the next
     * argument), -e (stop at the first command that fails) and -x (show
     * each command, with a "+ ", on stderr). make runs recipes as
     * `sh -ec 'command'`.
     */
    {
        int i = 1, cmd = 0;

        while (i < argc && args[i][0] == '-' && args[i][1]) {
            const char *o = args[i] + 1;

            if (strcmp(args[i], "--") == 0) {
                i++;
                break;
            }
            for (; *o; o++) {
                if (*o == 'c') {
                    cmd = 1;
                } else if (*o == 'e') {
                    opt_errexit = 1;
                } else if (*o == 'x') {
                    opt_xtrace = 1;
                } else {
                    char bad[2] = { *o, '\0' };

                    err_puts("sh: unknown option -");
                    err_puts(bad);
                    err_puts("\n");
                    return 2;
                }
            }
            i++;
        }
        if (cmd) {
            if (i >= argc) {
                err_puts("sh: -c needs a command\n");
                return 2;
            }
            if (strlen(args[i]) >= sizeof(line)) {
                err_puts("sh: command too long\n");
                return 2;
            }
            strcpy(line, args[i]);
            strncpy(cmdline_saved, line, sizeof(cmdline_saved) - 1);
            cmdline_saved[sizeof(cmdline_saved) - 1] = '\0';
            run_list(line);
            out_flush();
            return last_status;
        }
        args += i - 1;
        argc -= i - 1;
    }
    if (argc > 1) {
        int err = run_script(args[1]);

        if (err < 0) {
            err_report(args[1], err);
            return 127;
        }
        out_flush();
        return last_status;
    }
    interactive();
    return last_status;
}
