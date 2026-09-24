/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * whocommon.h - what who(1) and w(1) both need.
 *
 * They ask the machine the same question and format the same three
 * facts, so the parsing and the arithmetic live here rather than in
 * two copies that drift. `static inline` because each program is one
 * translation unit and neither wants a warning about the helper it
 * does not use.
 */
#ifndef WHOCOMMON_H
#define WHOCOMMON_H

#include "ulib.h"

/* ulib has strlen, strcmp, memcpy and memset, and no more than that. */
static inline void copy(char *dst, const char *src, int max)
{
    int i = 0;

    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static inline void num(u32 v, char *out)
{
    char tmp[16];
    int k = 0, j;

    if (v == 0) {
        tmp[k++] = '0';
    }
    while (v) {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    }
    for (j = 0; j < k; j++) {
        out[j] = tmp[k - 1 - j];
    }
    out[j] = '\0';
}

/*
 * uid -> login name from /etc/passwd, or the number if it says nothing.
 *
 * Parsed here rather than through getpwuid(), because this program is
 * built against lib/ulib and not the C library -- the same reason
 * id(1) carries its own parser.
 */
static inline void uid_name(u32 uid, char *out, int outlen)
{
    int fd = open("/etc/passwd", O_RDONLY);
    static char buf[4096];
    int n, i, start = 0;

    out[0] = '\0';
    if (fd >= 0) {
        n = read(fd, buf, (int)sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            for (i = 0; i <= n; i++) {
                char *p, *name;
                int f;

                if (buf[i] != '\n' && buf[i] != '\0') {
                    continue;
                }
                buf[i] = '\0';
                p = name = buf + start;
                start = i + 1;
                if (*name == '#' || *name == '\0') {
                    continue;
                }
                for (f = 0; *p; p++) {
                    if (*p != ':') {
                        continue;
                    }
                    *p = '\0';          /* ends the field before it */
                    if (++f == 2) {     /* field 2 is the uid */
                        u32 v = 0;
                        const char *q = p + 1;

                        while (*q >= '0' && *q <= '9') {
                            v = v * 10 + (u32)(*q++ - '0');
                        }
                        if (v == uid) {
                            copy(out, name, outlen);
                            return;
                        }
                        break;
                    }
                }
            }
        }
    }
    num(uid, out);              /* no name: the number, not nothing */
}

/*
 * Seconds since the epoch -> "YYYY-MM-DD HH:MM".
 *
 * Written out because lib/ulib has no localtime(). The arithmetic is
 * the civil-from-days algorithm: exact for every date this machine can
 * hold, and it needs no table. UTC, because the timezone is a shell
 * variable and a ulib program cannot see it.
 */
static inline void stamp(u32 t, char *out)
{
    u32 days = t / 86400, secs = t % 86400;
    u32 era, doe, yoe, y, doy, mp, d, m;
    int i = 0;

    days += 719468;                     /* move the epoch to 0000-03-01 */
    era = days / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) {
        y++;
    }

    out[i++] = (char)('0' + (y / 1000) % 10);
    out[i++] = (char)('0' + (y / 100) % 10);
    out[i++] = (char)('0' + (y / 10) % 10);
    out[i++] = (char)('0' + y % 10);
    out[i++] = '-';
    out[i++] = (char)('0' + m / 10);
    out[i++] = (char)('0' + m % 10);
    out[i++] = '-';
    out[i++] = (char)('0' + d / 10);
    out[i++] = (char)('0' + d % 10);
    out[i++] = ' ';
    out[i++] = (char)('0' + (secs / 3600) / 10);
    out[i++] = (char)('0' + (secs / 3600) % 10);
    out[i++] = ':';
    out[i++] = (char)('0' + (secs % 3600 / 60) / 10);
    out[i++] = (char)('0' + (secs % 3600 / 60) % 10);
    out[i] = '\0';
}

static inline void pad(const char *s, int w)
{
    int n = (int)strlen(s);

    puts(s);
    while (n++ < w) {
        puts(" ");
    }
}

/*
 * The login time of a session, as a wall clock.
 *
 * start is jiffies since boot; uptime is seconds since boot; now is
 * the wall clock. The session began (uptime - start/HZ) seconds ago,
 * whatever the clock has done since.
 */
static inline u32 who_login_time(u32 start, u32 uptime, u32 now)
{
    u32 age = uptime > start / HZ ? uptime - start / HZ : 0;

    return now > age ? now - age : now;
}

/*
 * THE TASK TABLE, read once.
 *
 * Both programs go over it more than once -- who(1) to reject a login
 * that a still older one on the same terminal already accounts for, and
 * w(1) to find what each login is running -- and a table that is read
 * again between those passes is a table that can change between them.
 * One snapshot keeps the answer self-consistent.
 */
#define WHO_MAX 64              /* TASK_MAX; a short read just ends */

static struct who_info who_tab[WHO_MAX];
static int who_n;

static inline void who_load(void)
{
    for (who_n = 0; who_n < WHO_MAX; who_n++) {
        if (syscall(__NR_jobctl, JOBCTL_WHO, who_n, &who_tab[who_n]) != 0) {
            break;
        }
    }
}

/*
 * IS THIS TASK SOMEBODY'S LOGIN?
 *
 * It is, if it is a login shell on a terminal and no older login shell
 * is sitting on the same terminal. See the note at the head of who.c
 * for why that is the rule and what the two obvious alternatives get
 * wrong.
 *
 * The second half is what `su -` costs: it starts a second login shell
 * on the terminal somebody is already logged in at, and counting that
 * as an arrival would have one person logged in twice. The OLDER one
 * wins, so the answer stays the person who logged in -- which is what a
 * system with a utmp reports, since su does not write one.
 */
static inline int who_is_login(int i)
{
    const struct who_info *w = &who_tab[i];
    int j;

    if (w->name[0] != '-' || w->tty[0] == '\0') {
        return 0;
    }
    for (j = 0; j < who_n; j++) {
        const struct who_info *o = &who_tab[j];

        if (j == i || o->name[0] != '-' || strcmp(o->tty, w->tty) != 0) {
            continue;
        }
        /* Older, or the same tick and the lower pid -- so that two
         * shells started in one jiffy still pick the same winner
         * whichever order the table hands them over. */
        if (o->start < w->start ||
            (o->start == w->start && o->pid < w->pid)) {
            return 0;
        }
    }
    return 1;
}

/* Is `pid` this task, or one of the things it started? */
static inline int who_descends_from(int pid, int ancestor)
{
    int hops;

    for (hops = 0; hops < WHO_MAX && pid > 0; hops++) {
        int j;

        if (pid == ancestor) {
            return 1;
        }
        for (j = 0; j < who_n && who_tab[j].pid != pid; j++) {
            ;
        }
        if (j == who_n) {
            return 0;           /* the parent is already gone */
        }
        pid = who_tab[j].ppid;
    }
    return 0;
}

#endif /* WHOCOMMON_H */
