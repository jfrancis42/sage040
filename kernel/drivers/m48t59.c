/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * m48t59.c - ST M48T59 TIMEKEEPER: the system clock, and 8 KiB of NVRAM.
 *
 * Reference: STMicroelectronics M48T59 datasheet.
 *
 * The part is one 8 KiB SRAM window whose last eight bytes are the
 * clock. Every time field is BCD, byte wide. There is no century
 * register, so the board supplies it -- RTC_BASE_YEAR in sage040.h --
 * which is why this chip covers 2000 to 2099 and not more.
 *
 * It registers as the system clock and answers in seconds since 1970.
 * Nothing above it sees BCD, a control register or a two-digit year;
 * putting an MC146818 here instead is a new file and one more call in
 * main.c.
 *
 * The datasheet's read and write protocol:
 *
 *   Reading: set R in the control register, which stops the clock's
 *   user-visible copy advancing while the seven bytes are read, then
 *   clear R. Without it a read can straddle a carry and produce a time
 *   that never happened -- seconds of 59 followed by a minutes read that
 *   has just rolled over reads an hour early.
 *
 *   Writing: set W, store the bytes, clear W, and clearing it is what
 *   transfers them into the counters.
 *
 * Both are implemented because this is meant to describe real hardware.
 * **Under QEMU neither bit does anything.** The model computes every
 * read live from the host clock and applies every write immediately.
 * Two consequences, both measured rather than assumed, and both covered
 * by tests/t11-rtc:
 *
 *   - The freeze does not freeze, so rtc_read() reads the clock twice
 *     and repeats if the seconds moved. That loop is what actually
 *     guarantees a consistent reading here, and it costs nothing on
 *     hardware where R works.
 *   - Because each field is applied as it is written, and applied
 *     through a normalising conversion, the obvious write order passes
 *     through dates that do not exist. Setting 29 February while the
 *     clock still holds a non-leap year silently becomes 1 March. The
 *     write below parks the day at 1 first so every intermediate state
 *     is a real date.
 */
#include "rtc.h"
#include "drivers.h"
#include "dev.h"
#include "time.h"
#include "errno.h"

#define READ_ATTEMPTS  3

static u8 bcd2bin(u8 v)
{
    return (u8)(((v >> 4) & 0x0f) * 10 + (v & 0x0f));
}

static u8 bin2bcd(u8 v)
{
    return (u8)(((v / 10) << 4) | (v % 10));
}

/* ---------------------------------------------------------------- */
/* The chip                                                          */
/* ---------------------------------------------------------------- */

static void read_raw(struct tm *t)
{
    MMIO8(RTC_CONTROL) = RTC_CTL_R;     /* freeze -- real hardware only */

    t->tm_sec  = bcd2bin(MMIO8(RTC_SECONDS) & 0x7f);
    t->tm_min  = bcd2bin(MMIO8(RTC_MINUTES) & 0x7f);
    t->tm_hour = bcd2bin(MMIO8(RTC_HOURS)   & 0x3f);
    t->tm_mday = bcd2bin(MMIO8(RTC_DATE)    & 0x3f);
    t->tm_mon  = bcd2bin(MMIO8(RTC_MONTH)   & 0x1f) - 1;
    t->tm_year = RTC_BASE_YEAR + bcd2bin(MMIO8(RTC_YEAR)) - 1900;

    MMIO8(RTC_CONTROL) = 0;             /* release */

    t->tm_wday = weekday_of(t->tm_year + 1900, t->tm_mon, t->tm_mday);
}

static int same_time(const struct tm *a, const struct tm *b)
{
    return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min &&
           a->tm_hour == b->tm_hour && a->tm_mday == b->tm_mday &&
           a->tm_mon == b->tm_mon && a->tm_year == b->tm_year;
}

int rtc_read(struct tm *t)
{
    struct tm a, b;
    int attempt;

    for (attempt = 0; attempt < READ_ATTEMPTS; attempt++) {
        read_raw(&a);
        read_raw(&b);
        if (same_time(&a, &b)) {
            *t = a;
            return tm_valid(t) ? 0 : -EIO;
        }
    }
    /* Three disagreements in a row is not a clock ticking, it is a clock
     * misbehaving. Report the last reading and say so. */
    *t = b;
    return -EIO;
}

int rtc_write(const struct tm *t)
{
    int year = t->tm_year + 1900;

    if (!tm_valid(t)) {
        return -EINVAL;
    }
    if (year < RTC_YEAR_MIN || year > RTC_YEAR_MAX) {
        return -EINVAL;
    }

    MMIO8(RTC_CONTROL) = RTC_CTL_W;

    /*
     * Bit 7 of the seconds register is ST, the stop bit. Writing the
     * seconds with it clear both sets the value and makes sure the
     * oscillator is running -- a part fresh from the factory ships
     * stopped, to save its battery.
     */
    MMIO8(RTC_SECONDS) = bin2bcd((u8)t->tm_sec);
    MMIO8(RTC_MINUTES) = bin2bcd((u8)t->tm_min);
    MMIO8(RTC_HOURS)   = bin2bcd((u8)t->tm_hour);

    /* Day first as 1, then year, then month, then the real day. See the
     * file header: every intermediate state has to be a date that
     * exists. */
    MMIO8(RTC_DATE)    = bin2bcd(1);
    MMIO8(RTC_YEAR)    = bin2bcd((u8)(year - RTC_BASE_YEAR));
    MMIO8(RTC_MONTH)   = bin2bcd((u8)(t->tm_mon + 1));
    MMIO8(RTC_DATE)    = bin2bcd((u8)t->tm_mday);
    MMIO8(RTC_WEEKDAY) = (u8)(weekday_of(year, t->tm_mon, t->tm_mday) + 1);

    MMIO8(RTC_CONTROL) = 0;
    return 0;
}

/* ---------------------------------------------------------------- */
/* NVRAM                                                             */
/* ---------------------------------------------------------------- */

u8 nvram_read(u32 offset)
{
    if (offset >= RTC_NVRAM_SIZE) {
        return 0;
    }
    return MMIO8(RTC_BASE + offset);
}

void nvram_write(u32 offset, u8 value)
{
    if (offset >= RTC_NVRAM_SIZE) {
        return;
    }
    MMIO8(RTC_BASE + offset) = value;
}

/*
 * Is the chip there?
 *
 * The time registers cannot answer that: a dead bus reads as zeroes, and
 * zeroes are a legal-looking BCD midnight. So the test writes and reads
 * back an NVRAM byte, restoring it afterwards, which nothing but real
 * memory can pass.
 */
int rtc_present(void)
{
    const u32 probe = RTC_NVRAM_SIZE - 1;
    u8 saved = nvram_read(probe);
    int ok;

    nvram_write(probe, 0xa5);
    ok = (nvram_read(probe) == 0xa5);
    nvram_write(probe, 0x5a);
    ok = ok && (nvram_read(probe) == 0x5a);
    nvram_write(probe, saved);

    return ok;
}

/*
 * /dev/nvram: the 8176 bytes below the clock's registers, as a file of
 * that size -- read, write and seek, and nothing past the end. Linux has
 * a /dev/nvram too (the PC's CMOS, 114 bytes); a program that uses one
 * uses this the same way. What goes in it is up to the programs:
 * /bin/nvram keeps settings there (see its header for the layout).
 *
 * Under QEMU the chip's RAM lasts as long as the QEMU process -- the
 * machine model gives it no backing file -- so it survives a reset of
 * the machine, which is what `shutdown` does without -no-reboot, and not
 * the emulator exiting. On the real part, a battery keeps it.
 */
static s32 nv_read(struct file *f, void *buf, u32 len)
{
    u8 *p = buf;
    u32 n = 0;

    while (n < len && f->pos < RTC_NVRAM_SIZE) {
        p[n++] = nvram_read(f->pos++);
    }
    return (s32)n;
}

static s32 nv_write(struct file *f, const void *buf, u32 len)
{
    const u8 *p = buf;
    u32 n = 0;

    if (f->pos >= RTC_NVRAM_SIZE && len > 0) {
        return -ENOSPC;
    }
    while (n < len && f->pos < RTC_NVRAM_SIZE) {
        nvram_write(f->pos++, p[n++]);
    }
    return (s32)n;
}

static s32 nv_lseek(struct file *f, s32 off, int whence)
{
    s32 base = whence == SEEK_SET ? 0 :
               whence == SEEK_CUR ? (s32)f->pos :
               whence == SEEK_END ? (s32)RTC_NVRAM_SIZE : -1;

    if (base < 0 || base + off < 0 || base + off > (s32)RTC_NVRAM_SIZE) {
        return -EINVAL;
    }
    f->pos = (u32)(base + off);
    return (s32)f->pos;
}

static int nv_close(struct file *f)
{
    (void)f;
    return 0;
}

static int nv_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR;
    st->st_size = RTC_NVRAM_SIZE;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static const struct file_ops nv_ops = {
    nv_read,
    nv_write,
    nv_lseek,
    0,                          /* no ioctl */
    nv_close,
    nv_fstat,
    0,                          /* poll: the default; see dev.h */
    0,                          /* truncate: it is the size it is */
    0,                          /* mmap: not memory to map */
};

static struct chardev nv_dev = { .name = "nvram", .ops = &nv_ops };

/* ---------------------------------------------------------------- */
/* The device the kernel sees                                        */
/* ---------------------------------------------------------------- */

static int m48t59_get(struct rtcdev *r, time_t *out)
{
    struct tm t;
    int err;

    (void)r;
    err = rtc_read(&t);
    if (err < 0) {
        return err;
    }
    *out = timegm(&t);
    return 0;
}

static int m48t59_set(struct rtcdev *r, time_t secs)
{
    struct tm t;

    (void)r;
    gmtime_r(secs, &t);
    return rtc_write(&t);
}

static struct rtcdev m48t59_dev = {
    "m48t59",
    m48t59_get,
    m48t59_set,
    0
};

int m48t59_init(void)
{
    /* Is the chip fitted? An address with nothing behind it raises a
     * bus error rather than reading back zeroes, so this has to be
     * asked before the first register access, not by making one. */
    if (!io_probe8((volatile void *)RTC_BASE)) {
        return -ENODEV;
    }

    if (!rtc_present()) {
        return -ENODEV;
    }
    dev_register_char(&nv_dev);
    return dev_register_rtc(&m48t59_dev);
}
