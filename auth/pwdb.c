/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pwdb.c - reading and writing the account files.
 *
 * REWRITING RATHER THAN EDITING IN PLACE. Every change here writes a
 * whole new file beside the old one and renames it over the top. The
 * alternative -- seeking to a field and overwriting it -- works only
 * while the replacement is exactly as long as what it replaces, which
 * a password hash never is, and leaves a half-written account file if
 * anything goes wrong in the middle. An account file is the one thing
 * on the machine that must not be half-written: lose it and nobody can
 * log in.
 *
 * The temporary file is created 0600 and renamed, so there is no
 * moment at which a world-readable copy of /etc/shadow exists.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include "pwdb.h"

#define PASSWD "/etc/passwd"
#define SHADOW "/etc/shadow"
#define GROUP  "/etc/group"

/*
 * One field of a colon-separated line, copied out.
 *
 * Returns a pointer past the separator, or 0 at the end of the line.
 * Comments and blank lines are the caller's business -- see skip().
 */
static char *field(char *p, char *out, unsigned len)
{
    unsigned i = 0;

    if (!p) {
        if (out && len) {
            out[0] = '\0';
        }
        return 0;
    }
    while (*p && *p != ':' && *p != '\n') {
        if (out && i + 1 < len) {
            out[i++] = *p;
        }
        p++;
    }
    if (out && len) {
        out[i] = '\0';
    }
    if (*p == ':') {
        return p + 1;
    }
    return 0;
}

/* A line that is not an entry: blank, or a comment. The C library's own
 * reader skips these (libc/patches/33) and so must this one, or one
 * note at the top of a file breaks every lookup below it. */
static int skip(const char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '\0' || *line == '\n' || *line == '#';
}

static int pw_find(const char *name, unsigned uid, int by_name,
                   struct pwent *out)
{
    FILE *f = fopen(PASSWD, "r");
    char line[PW_MAX_LINE], num[32];
    int found = 0;

    if (!f) {
        return -1;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        struct pwent e;
        char *p;

        if (skip(line)) {
            continue;
        }
        p = field(line, e.name, sizeof(e.name));
        p = field(p, 0, 0);                     /* the 'x' */
        p = field(p, num, sizeof(num));
        e.uid = (unsigned)strtoul(num, 0, 10);
        p = field(p, num, sizeof(num));
        e.gid = (unsigned)strtoul(num, 0, 10);
        p = field(p, e.gecos, sizeof(e.gecos));
        p = field(p, e.home, sizeof(e.home));
        (void)field(p, e.shell, sizeof(e.shell));
        if (by_name ? (strcmp(e.name, name) == 0) : (e.uid == uid)) {
            *out = e;
            found = 1;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int pw_by_name(const char *name, struct pwent *out)
{
    return pw_find(name, 0, 1, out);
}

int pw_by_uid(unsigned uid, struct pwent *out)
{
    return pw_find(0, uid, 0, out);
}

int sh_hash(const char *name, char *hash, unsigned len)
{
    FILE *f = fopen(SHADOW, "r");
    char line[PW_MAX_LINE], nm[PW_MAX_NAME];
    int found = 0;

    if (!f) {
        return -1;              /* not readable: not root */
    }
    while (!found && fgets(line, sizeof(line), f)) {
        char *p;

        if (skip(line)) {
            continue;
        }
        p = field(line, nm, sizeof(nm));
        if (strcmp(nm, name) == 0) {
            (void)field(p, hash, len);
            found = 1;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

/*
 * Rewrite one file, replacing the line whose first field is `name`.
 *
 * `repl` is the whole new line (without a newline), or 0 to delete the
 * entry. Returns 0 if something was changed.
 */
static int rewrite(const char *path, const char *name, const char *repl,
                   mode_t mode)
{
    char tmp[128];
    FILE *in, *out;
    char line[PW_MAX_LINE], nm[PW_MAX_NAME];
    int fd, hit = 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    in = fopen(path, "r");
    if (!in) {
        return -1;
    }
    /* 0600 from the start, then chmod to what it should be: a shadow
     * file must never exist even briefly with a readable mode. */
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fclose(in);
        return -1;
    }
    out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        fclose(in);
        return -1;
    }
    while (fgets(line, sizeof(line), in)) {
        if (skip(line)) {
            fputs(line, out);
            continue;
        }
        (void)field(line, nm, sizeof(nm));
        if (strcmp(nm, name) == 0) {
            hit = 1;
            if (repl) {
                fprintf(out, "%s\n", repl);
            }
            continue;           /* 0 repl: the entry is dropped */
        }
        fputs(line, out);
    }
    if (!hit && repl) {         /* a new entry goes at the end */
        fprintf(out, "%s\n", repl);
        hit = 1;
    }
    fclose(in);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
        fclose(out);
        unlink(tmp);
        return -1;
    }
    fclose(out);
    if (chmod(tmp, mode) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return hit ? 0 : -1;
}

int sh_set_hash(const char *name, const char *hash)
{
    char line[PW_MAX_LINE], old[PW_MAX_LINE], nm[PW_MAX_NAME];
    FILE *f = fopen(SHADOW, "r");
    int found = 0;

    /* The trailing fields are kept as they were: they are ageing
     * information and this is not the program that ages anything. */
    old[0] = '\0';
    if (f) {
        while (!found && fgets(line, sizeof(line), f)) {
            char *p;

            if (skip(line)) {
                continue;
            }
            p = field(line, nm, sizeof(nm));
            if (strcmp(nm, name) == 0) {
                p = field(p, 0, 0);             /* skip the old hash */
                if (p) {
                    unsigned n = (unsigned)strlen(p);

                    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) {
                        p[--n] = '\0';
                    }
                    snprintf(old, sizeof(old), "%s", p);
                }
                found = 1;
            }
        }
        fclose(f);
    }
    snprintf(line, sizeof(line), "%s:%s:%s", name, hash,
             found ? old : "20000:0:99999:7:::");
    return rewrite(SHADOW, name, line, 0600);
}

long gr_gid(const char *group)
{
    FILE *f = fopen(GROUP, "r");
    char line[PW_MAX_LINE], nm[PW_MAX_NAME], num[32];
    long gid = -1;

    if (!f) {
        return -1;
    }
    while (gid < 0 && fgets(line, sizeof(line), f)) {
        char *p;

        if (skip(line)) {
            continue;
        }
        p = field(line, nm, sizeof(nm));
        p = field(p, 0, 0);
        p = field(p, num, sizeof(num));
        (void)p;
        if (strcmp(nm, group) == 0) {
            gid = strtol(num, 0, 10);
        }
    }
    fclose(f);
    return gid;
}

int gr_of_user(const char *name, unsigned primary, gid_t *gids, int max)
{
    FILE *f;
    char line[PW_MAX_LINE], nm[PW_MAX_NAME], num[32];
    int n = 0;

    if (max > 0) {
        gids[n++] = (gid_t)primary;    /* the primary group counts and is first */
    }
    f = fopen(GROUP, "r");
    if (!f) {
        return n;
    }
    while (n < max && fgets(line, sizeof(line), f)) {
        char *p, *members;
        gid_t gid;
        int already, i;

        if (skip(line)) {
            continue;
        }
        p = field(line, nm, sizeof(nm));
        p = field(p, 0, 0);
        p = field(p, num, sizeof(num));
        gid = (gid_t)strtoul(num, 0, 10);
        members = p;
        if (!members) {
            continue;
        }
        /* The fourth field is a comma-separated list of names. A match
         * has to be a WHOLE name: "jeff" must not match "jeffrey". */
        {
            char *s = members, *e;
            int hit = 0;

            while (!hit && *s && *s != '\n') {
                e = s;
                while (*e && *e != ',' && *e != '\n') {
                    e++;
                }
                if ((unsigned)(e - s) == strlen(name) &&
                    strncmp(s, name, (size_t)(e - s)) == 0) {
                    hit = 1;
                }
                s = (*e == ',') ? e + 1 : e;
            }
            if (!hit) {
                continue;
            }
        }
        already = 0;
        for (i = 0; i < n; i++) {
            if (gids[i] == gid) {
                already = 1;
            }
        }
        if (!already) {
            gids[n++] = (gid_t)gid;
        }
    }
    fclose(f);
    return n;
}

unsigned pw_next_uid(unsigned from)
{
    struct pwent e;
    unsigned uid = from;

    while (pw_by_uid(uid, &e) == 0) {
        uid++;
    }
    return uid;
}

int pw_add(const struct pwent *p, const char *hash)
{
    char line[PW_MAX_LINE];

    snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s",
             p->name, p->uid, p->gid, p->gecos, p->home, p->shell);
    if (rewrite(PASSWD, p->name, line, 0644) != 0) {
        return -1;
    }
    snprintf(line, sizeof(line), "%s:%s:20000:0:99999:7:::",
             p->name, hash ? hash : "*");
    if (rewrite(SHADOW, p->name, line, 0600) != 0) {
        return -1;
    }
    snprintf(line, sizeof(line), "%s::%u:", p->name, p->gid);
    return rewrite(GROUP, p->name, line, 0644);
}

int pw_del(const char *name)
{
    int err = rewrite(PASSWD, name, 0, 0644);

    /* The shadow and group entries go whether or not they were there:
     * an account half-removed is worse than one removed twice. */
    (void)rewrite(SHADOW, name, 0, 0600);
    (void)rewrite(GROUP, name, 0, 0644);
    return err;
}

/*
 * Ask for a password with the echo turned off.
 *
 * The terminal's settings are put back whatever happens, including on
 * the error paths: a program that exits leaving echo off makes the
 * session look dead, and the person cannot see what they type to fix
 * it.
 */
int read_password(const char *prompt, char *buf, unsigned len)
{
    struct termios old, quiet;
    int have_tty = 0;
    unsigned n = 0;
    int c;

    if (tcgetattr(0, &old) == 0) {
        quiet = old;
        quiet.c_lflag &= ~(unsigned)ECHO;
        if (tcsetattr(0, TCSANOW, &quiet) == 0) {
            have_tty = 1;
        }
    }
    fputs(prompt, stdout);
    fflush(stdout);
    while ((c = getchar()) != EOF && c != '\n' && c != '\r') {
        if (c == '\b' || c == 0x7f) {
            if (n) {
                n--;
            }
            continue;
        }
        if (n + 1 < len) {
            buf[n++] = (char)c;
        }
    }
    buf[n] = '\0';
    if (have_tty) {
        tcsetattr(0, TCSANOW, &old);
    }
    fputs("\n", stdout);
    fflush(stdout);
    return (c == EOF && n == 0) ? -1 : 0;
}

void make_salt(char *out, unsigned len)
{
    static const char set[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char raw[16];
    unsigned i, n = 0;
    int fd;

    /*
     * FROM /dev/urandom, not from the clock. A salt taken from the time
     * is guessable, and two accounts made in the same second get the
     * same one -- which is exactly what a salt exists to prevent.
     */
    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        n = (unsigned)read(fd, raw, sizeof(raw));
        close(fd);
    }
    snprintf(out, len, "$6$");
    for (i = 0; i < 16 && i < n && 3 + i + 1 < len; i++) {
        out[3 + i] = set[raw[i] & 0x3f];
    }
    out[3 + i] = '\0';
}
