/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * klogd - copy what the kernel says into /var/log/syslog.
 *
 * The kernel keeps its messages in a ring (kernel/klog.c) and never
 * writes to a file: the first of them exist before there is a disk
 * driver, and a panic has to work with the filesystem in any state.
 * Moving them somewhere permanent is therefore a program's job, and
 * this is that program -- the same division Linux draws.
 *
 * It reads /dev/klog, which BLOCKS until the kernel has said something,
 * so this sits asleep rather than polling. Started from /etc/rc with
 * `klogd &`, it is a background task that costs nothing until a message
 * arrives.
 *
 * Each line gets a timestamp, in the shape syslog has used since the
 * 1980s -- except that the time is UTC and says so, because that is
 * what the machine's clock keeps and a log that quietly used the
 * current TZ would be unreadable after the zone changed:
 *
 *     2026-09-22 16:44:01Z sage040 kernel: eth0: link up
 *
 * WHAT IT DOES NOT DO: rotate the file, filter by priority, or listen
 * on a socket. Those are the parts of a syslog daemon that matter on a
 * machine with many programs logging; here there is one source, and the
 * file is read with `cat`.
 */
#include "ulib.h"

#define LOGDIR  "/var/log"
#define LOGFILE "/var/log/syslog"

/* Days in each month, and whether a year is a leap year: turning a
 * time_t into a date needs both, and ulib has no gmtime. */
static int leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int two(char *p, int v)
{
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
    return 2;
}

/*
 * "2026-09-22 16:44:01Z ", into buf, from the seconds since 1970 that
 * the clock keeps. Written out rather than called for, because the only
 * alternative on this library is to link the whole of a date library
 * for one line of output.
 */
static int stamp(char *buf, time_t now)
{
    static const int mdays[] = { 31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31 };
    u32 days = (u32)(now / 86400);
    u32 secs = (u32)(now % 86400);
    int year = 1970, mon = 0, n = 0;

    for (;;) {
        u32 len = leap(year) ? 366 : 365;

        if (days < len) {
            break;
        }
        days -= len;
        year++;
    }
    for (mon = 0; mon < 12; mon++) {
        u32 len = (u32)mdays[mon] + ((mon == 1 && leap(year)) ? 1 : 0);

        if (days < len) {
            break;
        }
        days -= len;
    }

    n += two(buf + n, year / 100);
    n += two(buf + n, year % 100);
    buf[n++] = '-';
    n += two(buf + n, mon + 1);
    buf[n++] = '-';
    n += two(buf + n, (int)days + 1);
    buf[n++] = ' ';
    n += two(buf + n, (int)(secs / 3600));
    buf[n++] = ':';
    n += two(buf + n, (int)((secs / 60) % 60));
    buf[n++] = ':';
    n += two(buf + n, (int)(secs % 60));
    buf[n++] = 'Z';
    buf[n++] = ' ';
    return n;
}

static int append(char *dst, int at, const char *s, int max)
{
    while (*s && at < max) {
        dst[at++] = *s++;
    }
    return at;
}

int main(int argc, char **argv)
{
    struct utsname u;
    char in[512];
    char line[512];
    char out[768];
    const char *node = "sage040";
    int  used = 0;              /* how much of `line` holds a message  */
    int  klog, log, quiet = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else {
            eputs("usage: klogd [-q]\n");
            return 2;
        }
    }

    klog = open("/dev/klog", O_RDONLY);
    if (klog < 0) {
        eputs("klogd: no /dev/klog\n");
        return 1;
    }

    /*
     * The directory first, and its parent: a machine whose disk has
     * never had one should start logging rather than fail.
     */
    mkdir("/var");
    mkdir(LOGDIR);
    log = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND);
    if (log < 0) {
        eputs("klogd: cannot write " LOGFILE "\n");
        return 1;
    }
    if (uname(&u) == 0 && u.nodename[0]) {
        node = u.nodename;
    }
    if (!quiet) {
        puts("klogd: logging the kernel to " LOGFILE "\n");
    }

    /*
     * A line at a time: the ring holds bytes, and a message is not
     * necessarily whole when it arrives. Anything still in hand when
     * the next read comes stays there -- which is why a message split
     * across two reads is logged once, not twice.
     */
    for (;;) {
        s32 n = read(klog, in, sizeof(in));
        s32 k;

        if (n <= 0) {
            break;              /* the device went away: nothing to do */
        }
        for (k = 0; k < n; k++) {
            char c = in[k];

            if (c == '\r') {
                continue;
            }
            if (c != '\n') {
                if (used < (int)sizeof(line)) {
                    line[used++] = c;
                }
                continue;
            }
            if (used > 0) {
                int m = stamp(out, time(0));

                m = append(out, m, node, (int)sizeof(out) - 16);
                m = append(out, m, " kernel: ", (int)sizeof(out) - 8);
                if (used > (int)sizeof(out) - m - 2) {
                    used = (int)sizeof(out) - m - 2;
                }
                memcpy(out + m, line, (u32)used);
                m += used;
                out[m++] = '\n';
                write(log, out, (u32)m);
                used = 0;
            }
        }
    }
    close(log);
    close(klog);
    return 0;
}
