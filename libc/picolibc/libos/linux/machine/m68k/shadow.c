/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2026 Jeff Francis */
/*
 * shadow.c - reading /etc/shadow.
 *
 * The file is 0600 root, so every one of these answers NULL for an
 * ordinary user and sets errno to whatever open(2) said -- EACCES,
 * normally. THAT IS THE CORRECT ANSWER, not a failure to work around:
 * a library that found some other way to the hash would defeat the
 * only thing the separate file is for.
 *
 * Callers must tell the two cases apart. NULL with ENOENT means "no
 * entry for that user"; NULL with EACCES means "you may not look".
 * Treating the second as the first turns "run me as root" into
 * "incorrect password", which is a maddening thing to debug -- see
 * dropbear, which falls back to pw_passwd and then refuses every
 * password if this is read by the wrong user.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <shadow.h>

#define LINE_MAX_SP 1024

static FILE *spf;
static struct spwd sp_entry;
static char sp_line[LINE_MAX_SP];

/* One colon-separated field, in place: the line is chopped up and the
 * struct points into it, which is what every getpwent-shaped function
 * does and why the result is only valid until the next call. */
static char *chop(char **p)
{
    char *s = *p, *e;

    if (!s) {
        return 0;
    }
    e = strchr(s, ':');
    if (e) {
        *e = '\0';
        *p = e + 1;
    } else {
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r')) {
            *--e = '\0';
        }
        *p = 0;
    }
    return s;
}

static long num(char *s)
{
    if (!s || *s == '\0') {
        return -1;              /* an empty ageing field means "unset" */
    }
    return strtol(s, 0, 10);
}

static struct spwd *parse(char *line)
{
    char *p = line;

    /* A blank line or a comment is not an entry. The passwd reader
     * skips both (libc/patches/33) and so must this one. */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '#') {
        return 0;
    }
    sp_entry.sp_namp = chop(&p);
    sp_entry.sp_pwdp = chop(&p);
    if (!sp_entry.sp_namp || !sp_entry.sp_pwdp) {
        return 0;               /* too few fields to be an entry */
    }
    sp_entry.sp_lstchg = num(chop(&p));
    sp_entry.sp_min    = num(chop(&p));
    sp_entry.sp_max    = num(chop(&p));
    sp_entry.sp_warn   = num(chop(&p));
    sp_entry.sp_inact  = num(chop(&p));
    sp_entry.sp_expire = num(chop(&p));
    sp_entry.sp_flag   = 0;
    return &sp_entry;
}

void setspent(void)
{
    if (spf) {
        rewind(spf);
    } else {
        spf = fopen(SHADOW, "r");
    }
}

void endspent(void)
{
    if (spf) {
        fclose(spf);
        spf = 0;
    }
}

struct spwd *getspent(void)
{
    if (!spf) {
        spf = fopen(SHADOW, "r");
        if (!spf) {
            return 0;           /* errno is open's: EACCES or ENOENT */
        }
    }
    while (fgets(sp_line, sizeof(sp_line), spf)) {
        struct spwd *e = parse(sp_line);

        if (e) {
            return e;
        }
    }
    return 0;
}

struct spwd *getspnam(const char *name)
{
    FILE *f;
    struct spwd *found = 0;

    if (!name) {
        errno = EINVAL;
        return 0;
    }
    /* A file of its own rather than the getspent cursor: a caller that
     * is part-way through a walk must not have it moved underneath
     * them by a lookup. */
    f = fopen(SHADOW, "r");
    if (!f) {
        return 0;               /* errno says which: EACCES or ENOENT */
    }
    while (!found && fgets(sp_line, sizeof(sp_line), f)) {
        struct spwd *e = parse(sp_line);

        if (e && strcmp(e->sp_namp, name) == 0) {
            found = e;
        }
    }
    fclose(f);
    if (!found) {
        errno = ENOENT;
    }
    return found;
}

int getspnam_r(const char *name, struct spwd *result, char *buf,
               size_t buflen, struct spwd **resultp)
{
    struct spwd *e;
    size_t n;

    *resultp = 0;
    e = getspnam(name);
    if (!e) {
        return errno ? errno : ENOENT;
    }
    /* The strings are copied into the caller's buffer, which is the
     * whole difference from getspnam: the result outlives the next
     * call. */
    n = strlen(e->sp_namp) + strlen(e->sp_pwdp) + 2;
    if (n > buflen) {
        return ERANGE;
    }
    *result = *e;
    result->sp_namp = buf;
    strcpy(buf, e->sp_namp);
    buf += strlen(e->sp_namp) + 1;
    result->sp_pwdp = buf;
    strcpy(buf, e->sp_pwdp);
    *resultp = result;
    return 0;
}
