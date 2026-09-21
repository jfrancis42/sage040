/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * time.h - calendar arithmetic.
 *
 * No hardware here. The kernel counts `time_t`, seconds since
 * 1970-01-01 UTC, and this is what turns that into a date and back.
 * Clock drivers convert between their own registers and `time_t`; every
 * other part of the system -- the filesystem's timestamps, the shell's
 * date command -- works in `time_t` and calls these.
 *
 * Which means a clock chip that stores BCD with a two-digit year and one
 * that stores a binary seconds counter both disappear behind the same
 * interface, and neither leaks upward.
 */
#ifndef TIME_H
#define TIME_H

#include "kernel.h"

/* Named as POSIX names it, and with the same fields, minus the ones
 * nothing here has a use for. */
struct tm {
    int tm_sec;                 /* 0-59                               */
    int tm_min;                 /* 0-59                               */
    int tm_hour;                /* 0-23                               */
    int tm_mday;                /* 1-31                               */
    int tm_mon;                 /* 0-11  -- POSIX numbers months from 0 */
    int tm_year;                /* years since 1900, as POSIX has it  */
    int tm_wday;                /* 0-6, Sunday is 0                   */
};

/* POSIX spells these gmtime() and timegm(); the r suffix is the version
 * that writes through a caller's struct rather than a static one, which
 * is the only sane choice in a kernel. */
void   gmtime_r(time_t secs, struct tm *out);
time_t timegm(const struct tm *t);

int    tm_valid(const struct tm *t);
int    days_in_month(int year, int mon0);   /* mon0 is 0-11           */
int    weekday_of(int year, int mon0, int mday);
int    is_leap(int year);

const char *weekday_name(int wday);
const char *month_name(int mon0);

/* MS-DOS packed date and time, which FAT directory entries hold. */
u16    tm_to_fat_date(const struct tm *t);
u16    tm_to_fat_time(const struct tm *t);
void   fat_to_tm(u16 date, u16 time, struct tm *out);

#endif /* TIME_H */
