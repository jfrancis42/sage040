/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * t11-rtc.c - ST M48T59 TIMEKEEPER: clock and NVRAM.
 *
 * Reference: STMicroelectronics M48T59 datasheet.
 *
 * The part is an 8 KiB SRAM whose last eight bytes are the clock, with
 * the alarm and watchdog registers in the eight below them.  Both halves
 * are checked here: that the NVRAM is real memory, that it does not
 * alias onto the clock, that the clock reads a sane date, that it is
 * actually running, and that a written time comes back.
 *
 * Every time field is BCD, and all accesses are byte wide.
 */
#include "sage040.h"

#define NVRAM_LAST  (RTC_NVRAM_SIZE - 1)

static u8 bcd2bin(u8 v)
{
    return (u8)(((v >> 4) & 0x0f) * 10 + (v & 0x0f));
}

static u8 bin2bcd(u8 v)
{
    return (u8)(((v / 10) << 4) | (v % 10));
}

static u8 rd(u32 addr)
{
    return MMIO8(addr);
}

static void put2(u32 v)
{
    uart_putc((char)('0' + (v / 10) % 10));
    uart_putc((char)('0' + v % 10));
}

int main(void)
{
    u8 saved[4];
    u8 sec, min, hour, day, mon, year;
    u8 sec2;
    int i;
    long spin;

    test_begin("t11 M48T59 clock and NVRAM");

    /* --- the NVRAM must be real memory ---------------------------- */
    {
        int ok = 1;
        static const u32 spots[4] = { 0, 1, RTC_NVRAM_SIZE / 2, NVRAM_LAST };

        for (i = 0; i < 4; i++) {
            saved[i] = rd(RTC_BASE + spots[i]);
        }
        /* A different value at each spot, so an address line stuck low
         * shows up as the wrong value rather than the right one. */
        for (i = 0; i < 4; i++) {
            MMIO8(RTC_BASE + spots[i]) = (u8)(0xa0 | i);
        }
        for (i = 0; i < 4; i++) {
            if (rd(RTC_BASE + spots[i]) != (u8)(0xa0 | i)) {
                ok = 0;
            }
        }
        if (ok) {
            test_ok("NVRAM holds distinct values at four addresses");
        } else {
            test_fail("NVRAM did not read back what was written");
        }

        for (i = 0; i < 4; i++) {
            MMIO8(RTC_BASE + spots[i]) = saved[i];
        }
    }

    /* --- writing NVRAM must not disturb the clock ------------------ */
    {
        u8 before = rd(RTC_MONTH);

        MMIO8(RTC_BASE + NVRAM_LAST) = 0x5a;
        if (rd(RTC_MONTH) == before) {
            test_ok("the top of NVRAM does not alias onto the clock");
        } else {
            test_fail("writing NVRAM changed a clock register");
        }
        MMIO8(RTC_BASE + NVRAM_LAST) = saved[3];
    }

    /* --- the clock should read a plausible date -------------------- */
    MMIO8(RTC_CONTROL) = RTC_CTL_R;         /* freeze for the read */
    sec  = bcd2bin(rd(RTC_SECONDS) & 0x7f);
    min  = bcd2bin(rd(RTC_MINUTES) & 0x7f);
    hour = bcd2bin(rd(RTC_HOURS)   & 0x3f);
    day  = bcd2bin(rd(RTC_DATE)    & 0x3f);
    mon  = bcd2bin(rd(RTC_MONTH)   & 0x1f);
    year = bcd2bin(rd(RTC_YEAR));
    MMIO8(RTC_CONTROL) = 0;

    uart_puts("  clock       = ");
    uart_putdec(RTC_BASE_YEAR + year);
    uart_putc('-'); put2(mon);
    uart_putc('-'); put2(day);
    uart_putc(' '); put2(hour);
    uart_putc(':'); put2(min);
    uart_putc(':'); put2(sec);
    uart_putc('\n');

    if (mon >= 1 && mon <= 12 && day >= 1 && day <= 31 &&
        hour <= 23 && min <= 59 && sec <= 59) {
        test_ok("every field is in range, so the registers are BCD");
    } else {
        test_fail("a field is out of range - BCD conversion or wiring");
    }

    /*
     * --- and it should be running -----------------------------------
     *
     * Waiting for the seconds register to change takes up to a full
     * second of wall time, and the only thing bounding this loop is a
     * count of MMIO reads. How long that count takes depends entirely on
     * the host: 20 million reads is several seconds on a slow one and
     * close to a single second on a fast one -- which made this check
     * fail intermittently, on nothing but the host being quick.
     *
     * 200 million is far past any plausible second, and it only ever
     * runs to the end when the clock has genuinely stopped.
     */
    sec2 = sec;
    for (spin = 0; spin < 200000000L; spin++) {
        sec2 = bcd2bin(rd(RTC_SECONDS) & 0x7f);
        if (sec2 != sec) {
            break;
        }
    }
    uart_puts("  seconds     = "); uart_putdec(sec);
    uart_puts(" then "); uart_putdec(sec2); uart_putc('\n');

    if (sec2 != sec) {
        test_ok("the seconds register advanced - the oscillator runs");
    } else {
        test_fail("the clock is not ticking");
    }

    /* --- a written time must come back ----------------------------- */
    {
        u8 r_hour, r_min, r_day, r_mon, r_year;

        MMIO8(RTC_CONTROL) = RTC_CTL_W;
        MMIO8(RTC_SECONDS) = bin2bcd(30);    /* bit 7 clear: keep running */
        MMIO8(RTC_MINUTES) = bin2bcd(45);
        MMIO8(RTC_HOURS)   = bin2bcd(13);
        /*
         * Day, then year, then month, then day again.  QEMU applies each
         * write immediately through a normalising conversion instead of
         * buffering until W is cleared, so the obvious order passes
         * through 29 February of whatever year the clock already held --
         * and if that year is not a leap year the date silently becomes
         * 1 March.  Parking the day at 1 first keeps every intermediate
         * state a date that exists.
         */
        MMIO8(RTC_DATE)    = bin2bcd(1);
        MMIO8(RTC_YEAR)    = bin2bcd(24);    /* 2024: a real leap year */
        MMIO8(RTC_MONTH)   = bin2bcd(2);
        MMIO8(RTC_DATE)    = bin2bcd(29);
        MMIO8(RTC_CONTROL) = 0;

        MMIO8(RTC_CONTROL) = RTC_CTL_R;
        r_min  = bcd2bin(rd(RTC_MINUTES) & 0x7f);
        r_hour = bcd2bin(rd(RTC_HOURS)   & 0x3f);
        r_day  = bcd2bin(rd(RTC_DATE)    & 0x3f);
        r_mon  = bcd2bin(rd(RTC_MONTH)   & 0x1f);
        r_year = bcd2bin(rd(RTC_YEAR));
        MMIO8(RTC_CONTROL) = 0;

        uart_puts("  read back   = ");
        uart_putdec(RTC_BASE_YEAR + r_year);
        uart_putc('-'); put2(r_mon);
        uart_putc('-'); put2(r_day);
        uart_putc(' '); put2(r_hour);
        uart_putc(':'); put2(r_min);
        uart_putc('\n');

        /* Seconds are deliberately not compared: the clock is running,
         * so they may legitimately have moved on between the write and
         * the read.  Everything coarser than a second must match. */
        if (r_year == 24 && r_mon == 2 && r_day == 29 &&
            r_hour == 13 && r_min == 45) {
            test_ok("2024-02-29 13:45 was stored and read back");
        } else {
            test_fail("the written time did not come back");
        }
    }

    /* --- 29 February must survive, and 30 February must not -------- */
    {
        u8 r_day, r_mon;

        MMIO8(RTC_CONTROL) = RTC_CTL_W;
        MMIO8(RTC_DATE) = bin2bcd(30);
        MMIO8(RTC_CONTROL) = 0;

        MMIO8(RTC_CONTROL) = RTC_CTL_R;
        r_day = bcd2bin(rd(RTC_DATE) & 0x3f);
        r_mon = bcd2bin(rd(RTC_MONTH) & 0x1f);
        MMIO8(RTC_CONTROL) = 0;

        uart_puts("  after 30 Feb= ");
        put2(r_mon); uart_putc('-'); put2(r_day); uart_putc('\n');

        /*
         * Nothing validates a date on the way in, so 30 February becomes
         * 1 March.  This records what the hardware does rather than what
         * it ought to, and it is the reason rtc_set() checks the date
         * against the month before writing anything: the chip will take
         * a date that does not exist and hand back a different one.
         */
        if (r_mon == 3 && r_day == 1) {
            test_ok("30 February rolls into 1 March, as the part does");
        } else {
            test_fail("30 February produced neither 29 Feb nor 1 March");
        }
    }

    test_end();
    return 0;
}
