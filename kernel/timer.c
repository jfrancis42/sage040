/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * timer.c - jiffies, and sleeping on them.
 */
#include "timer.h"
#include "dev.h"
#include "tty.h"
#include "task.h"
#include "tty.h"
#include "net.h"
#include "errno.h"

volatile u32 jiffies;

/* --- the clock -------------------------------------------------------- */

/*
 * ONE clock, for time(), gettimeofday() and file timestamps alike, so
 * that no two of them can disagree.
 *
 * The RTC says what second it is and nothing finer; the tick counts
 * hundredths but knows nothing of the date. So the clock is the RTC's
 * second, taken once, plus the ticks since. Taken at the moment the
 * RTC's second CHANGES, which the tick watches for during the first
 * second after boot -- otherwise every time would be up to a second
 * out, and the fraction gettimeofday() reports would be fiction.
 *
 * Until that moment the RTC is read directly, with no fraction.
 */
static u32 clock_base_sec;          /* the RTC's second ...            */
static u32 clock_base_jiffies;      /* ... began at this tick          */
static int clock_state;             /* 0 unknown, 1 watching, 2 set    */
static u32 clock_watch_sec;

static int rtc_now(u32 *secs)
{
    struct rtcdev *r = dev_rtc();
    time_t t;

    if (!r || r->get(r, &t) != 0) {
        return -1;
    }
    *secs = (u32)t;
    return 0;
}

/* From the tick, until the clock is set. */
static void clock_align(void)
{
    u32 s;

    if (clock_state == 2 || rtc_now(&s) < 0) {
        return;
    }
    if (clock_state == 0) {
        clock_watch_sec = s;
        clock_state = 1;
    } else if (s != clock_watch_sec) {
        clock_base_sec = s;
        clock_base_jiffies = jiffies;
        clock_state = 2;
    }
}

void clock_get(struct timeval *tv)
{
    u32 elapsed;

    if (clock_state != 2) {
        u32 s = 0;

        rtc_now(&s);
        tv->tv_sec = (s32)s;
        tv->tv_usec = 0;
        return;
    }
    elapsed = jiffies - clock_base_jiffies;
    tv->tv_sec = (s32)(clock_base_sec + elapsed / HZ);
    tv->tv_usec = (s32)((elapsed % HZ) * (1000000 / HZ));
}

int clock_set(const struct timeval *tv)
{
    struct rtcdev *r = dev_rtc();
    int err;

    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
        return -EINVAL;
    }
    if (!r) {
        return -ENODEV;
    }
    err = r->set(r, (time_t)tv->tv_sec);
    if (err < 0) {
        return err;
    }
    /* The fraction goes into where the second began. */
    clock_base_sec = (u32)tv->tv_sec;
    clock_base_jiffies = jiffies - (u32)tv->tv_usec / (1000000 / HZ);
    clock_state = 2;
    return 0;
}

/*
 * Called from the timer driver's interrupt handler, a hundred times a
 * second. Counting is the obvious part; the rest is everything on this
 * machine that has to happen whether or not anybody is asking -- see
 * each call below.
 */
void timer_tick(void)
{
    jiffies++;

    clock_align();

    /*
     * Empty the network card, whatever else is going on.
     *
     * Not protocol work -- that happens in net_poll(), in ordinary
     * kernel context. This only moves frames off the card into memory,
     * and it has to happen continuously rather than when something is
     * waiting: the LAN91C111 allocates transmit buffers from the same
     * pool that holds arriving frames, so a receiver that is never
     * drained stops the machine being able to SEND. On a real LAN that
     * takes seconds.
     */
    net_drain();

    /*
     * The terminal is polled here too: the UART and the keyboard have
     * interrupt lines that are not enabled, so this is what notices a
     * keystroke at all. It is also what spots ctrl-C while a program is
     * running -- the program is not reading, so nothing else would --
     * and what wakes anything asleep waiting for input.
     */
    tty_poll_signals();

    /* Anything whose sleep has run out of time. */
    task_timeouts();

    /*
     * And the running task's turn. This only MARKS it -- the switch
     * happens at the next return to user mode, because switching from
     * inside an interrupt would mean switching out of whatever the
     * kernel was in the middle of.
     */
    task_tick();
}

u32 timer_jiffies(void)
{
    return jiffies;
}

u32 timer_uptime_ms(void)
{
    return jiffies * (1000 / HZ);
}

int timer_start(void)
{
    struct timerdev *t = dev_timer();

    if (!t) {
        return -ENODEV;
    }
    return t->start(t, HZ);
}

/*
 * Wait for `ticks` of them to go by.
 *
 * STOP puts the CPU to sleep at IPL 0 until an interrupt arrives, which
 * beats spinning: under an emulator a spin burns the host's CPU for
 * nothing, and on real hardware it would burn actual power. The timer
 * interrupt is what wakes it.
 *
 * The comparison is signed on purpose. jiffies wraps after 497 days at
 * 100 Hz, and `jiffies < target` would then wait for the better part of
 * a year; `(s32)(jiffies - target) < 0` stays correct across the wrap.
 */
int timer_sleep_ticks(u32 ticks)
{
    u32 target;

    if (!dev_timer()) {
        return -ENODEV;
    }
    target = jiffies + ticks;
    while ((s32)(jiffies - target) < 0) {
        __asm__ volatile ("stop #0x2000");
    }
    return 0;
}

int timer_sleep_ms(u32 ms)
{
    /* Round up: a sleep that returns early is a bug waiting to happen,
     * and one tick late is nothing. */
    return timer_sleep_ticks((ms * HZ + 999) / 1000);
}
