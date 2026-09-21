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

/*
 * Called from the timer driver's interrupt handler.
 *
 * Counting is the obvious half. The other half is why ctrl-C works at
 * all on a program that never calls the kernel: nobody is reading the
 * keyboard while a cube spins, so nobody would ever see the keystroke.
 * A hundred times a second this looks for one, and if it is the
 * interrupt character the program is unwound from here.
 *
 * Only while a program is running, and only while the kernel is not in
 * the middle of a system call -- an unwind from inside one would leave
 * whatever it was doing half done. In every other case this is two
 * comparisons and a return.
 */
void timer_tick(void)
{
    jiffies++;

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
