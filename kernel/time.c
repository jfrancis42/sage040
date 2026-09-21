/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * time.c - calendar arithmetic, and nothing else.
 *
 * The conversion between a day number and a date is Howard Hinnant's
 * days_from_civil: shift the year to begin in March so that the leap day
 * becomes the last day of the year and the month-length pattern repeats
 * cleanly, then count whole 400-year eras. It needs no loop and no table
 * and is exact for every date this system can represent, which a
 * day-counting loop is not once someone tries a date a century away.
 */
#include "time.h"

#define SECS_PER_DAY  86400UL

int is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int days_in_month(int year, int mon0)
{
    static const int days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (mon0 < 0 || mon0 > 11) {
        return 0;
    }
    if (mon0 == 1 && is_leap(year)) {
        return 29;
    }
    return days[mon0];
}

/* Sakamoto's method. Sunday is 0. */
int weekday_of(int year, int mon0, int mday)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year;

    if (mon0 < 0 || mon0 > 11) {
        return 0;
    }
    if (mon0 < 2) {
        y--;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[mon0] + mday) % 7;
}

static s32 days_from_civil(int y, int m, int d)
{
    s32 era, yoe, doy, doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                /* 0..399     */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;        /* 0..146096  */
    return era * 146097 + doe - 719468;
}

static void civil_from_days(s32 z, int *y, int *m, int *d)
{
    s32 era, doe, yoe, doy, mp;
    s32 yr;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yr  = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m  = (int)(mp + (mp < 10 ? 3 : -9));
    *y  = (int)(yr + (*m <= 2));
}

void gmtime_r(time_t secs, struct tm *out)
{
    u32 days = secs / SECS_PER_DAY;
    u32 rem  = secs % SECS_PER_DAY;
    int y, m, d;

    civil_from_days((s32)days, &y, &m, &d);

    out->tm_year = y - 1900;
    out->tm_mon  = m - 1;
    out->tm_mday = d;
    out->tm_hour = (int)(rem / 3600);
    out->tm_min  = (int)((rem / 60) % 60);
    out->tm_sec  = (int)(rem % 60);
    out->tm_wday = weekday_of(y, m - 1, d);
}

time_t timegm(const struct tm *t)
{
    s32 days = days_from_civil(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    return (time_t)((u32)days * SECS_PER_DAY +
                    (u32)t->tm_hour * 3600 +
                    (u32)t->tm_min * 60 +
                    (u32)t->tm_sec);
}

int tm_valid(const struct tm *t)
{
    int year = t->tm_year + 1900;

    if (t->tm_mon < 0 || t->tm_mon > 11) {
        return 0;
    }
    if (t->tm_mday < 1 || t->tm_mday > days_in_month(year, t->tm_mon)) {
        return 0;
    }
    if (t->tm_hour < 0 || t->tm_hour > 23) {
        return 0;
    }
    if (t->tm_min < 0 || t->tm_min > 59) {
        return 0;
    }
    return t->tm_sec >= 0 && t->tm_sec <= 59;
}

const char *weekday_name(int wday)
{
    static const char *const names[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };

    return (wday >= 0 && wday < 7) ? names[wday] : "?";
}

const char *month_name(int mon0)
{
    static const char *const names[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    return (mon0 >= 0 && mon0 < 12) ? names[mon0] : "?";
}

/* ---------------------------------------------------------------- */
/* MS-DOS packed date and time                                       */
/* ---------------------------------------------------------------- */

u16 tm_to_fat_date(const struct tm *t)
{
    int year = t->tm_year + 1900;

    /* FAT counts years from 1980 in seven bits, so it runs out in 2107
     * and cannot express anything before 1980 at all. */
    if (year < 1980 || year > 2107) {
        return 0;
    }
    return (u16)(((year - 1980) << 9) | ((t->tm_mon + 1) << 5) | t->tm_mday);
}

u16 tm_to_fat_time(const struct tm *t)
{
    /* Seconds are stored in units of two: whoever designed this wanted
     * the bit more than the resolution. */
    return (u16)((t->tm_hour << 11) | (t->tm_min << 5) | (t->tm_sec / 2));
}

void fat_to_tm(u16 date, u16 time, struct tm *out)
{
    out->tm_year = 1980 + ((date >> 9) & 0x7f) - 1900;
    out->tm_mon  = ((date >> 5) & 0x0f) - 1;
    out->tm_mday = date & 0x1f;
    out->tm_hour = (time >> 11) & 0x1f;
    out->tm_min  = (time >> 5) & 0x3f;
    out->tm_sec  = (time & 0x1f) * 2;
    out->tm_wday = weekday_of(out->tm_year + 1900, out->tm_mon, out->tm_mday);
}
