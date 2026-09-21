/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * rtc.h - the M48T59 driver's own interface.
 *
 * Only main.c and the driver itself should need this. Everything else
 * asks the clock through `struct rtcdev` (dev.h) and gets a `time_t`,
 * which is the whole point: nothing above the driver knows there is a
 * battery-backed SRAM with BCD registers at the top of it.
 *
 * The NVRAM accessors are here because they are genuinely this part's
 * and have no device class of their own yet. 8176 bytes that survive a
 * power cycle is the only storage on this machine that is not the disk,
 * and it will earn a proper home when something uses it.
 */
#ifndef DRIVER_RTC_H
#define DRIVER_RTC_H

#include "kernel.h"
#include "time.h"

/*
 * The chip holds two BCD year digits and the board supplies the century,
 * so the range is 2000-2099. That is a property of the part, not a
 * shortcut: the M48T59 has no century register at all, and every system
 * built around one had to decide this somewhere.
 */
#define RTC_YEAR_MIN  RTC_BASE_YEAR
#define RTC_YEAR_MAX  (RTC_BASE_YEAR + 99)

int  m48t59_init(void);                 /* probe and register it       */
int  rtc_present(void);
int  rtc_read(struct tm *t);            /* 0 or -errno                 */
int  rtc_write(const struct tm *t);

u8   nvram_read(u32 offset);
void nvram_write(u32 offset, u8 value);

#endif /* DRIVER_RTC_H */
