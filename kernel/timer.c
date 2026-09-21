/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * timer.c - jiffies, and sleeping on them.
 */
#include "timer.h"
#include "dev.h"
#include "errno.h"

volatile u32 jiffies;

void timer_tick(void)
{
    jiffies++;
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
