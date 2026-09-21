/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * wait.c - queues, semaphores, mutexes.
 */
#include "wait.h"
#include "task.h"
#include "timer.h"
#include "errno.h"
#include "string.h"

u16 irq_save(void)
{
    u16 sr;

    __asm__ volatile ("move.w %%sr,%0\n\t"
                      "ori.w  #0x0700,%%sr"
                      : "=d"(sr) :: "cc", "memory");
    return sr;
}

void irq_restore(u16 sr)
{
    __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "cc", "memory");
}

/* --- queues ---------------------------------------------------------- */

static void enqueue(struct waitq *q, struct task *t)
{
    struct task **p = &q->head;

    /* At the tail, so that a queue is first-come-first-served. A stack
     * would be one line shorter and would starve whoever arrived first
     * under load, which is exactly when it would matter. */
    while (*p) {
        p = &(*p)->wait_next;
    }
    *p = t;
    t->wait_next = 0;
    t->queue = q;
}

static void dequeue(struct waitq *q, struct task *t)
{
    struct task **p = &q->head;

    while (*p) {
        if (*p == t) {
            *p = t->wait_next;
            t->wait_next = 0;
            t->queue = 0;
            return;
        }
        p = &(*p)->wait_next;
    }
}

void sleep_on(struct waitq *q)
{
    u16 sr = irq_save();

    /*
     * Masked from here until schedule() switches away. A wakeup arriving
     * in this window would otherwise find a task that had decided to
     * sleep but was not yet asleep, mark it ready, and then be undone by
     * the line below -- and the task would wait for an event that had
     * already happened.
     */
    enqueue(q, current);
    current->state = TASK_BLOCKED;

    schedule();

    /*
     * Back again. If something woke this task it is already off the
     * queue; if it got here another way -- a signal -- it may not be.
     */
    if (current->queue == q) {
        dequeue(q, current);
    }
    irq_restore(sr);
}

int sleep_on_timeout(struct waitq *q, u32 ms)
{
    u32 deadline = timer_jiffies() + (ms * HZ + 999) / 1000;
    u16 sr;

    for (;;) {
        sr = irq_save();
        /* Cleared before sleeping, not after: a flag left set by an
         * earlier wake would make this return immediately and report a
         * wakeup that has already been consumed. */
        current->woken = 0;
        enqueue(q, current);
        current->state = TASK_BLOCKED;
        current->wake_at = deadline;
        schedule();
        if (current->queue == q) {
            dequeue(q, current);
        }
        current->wake_at = 0;
        irq_restore(sr);

        if (current->woken) {
            current->woken = 0;
            return 1;
        }
        if ((s32)(timer_jiffies() - deadline) >= 0) {
            return 0;
        }
    }
}

static void wake_task(struct task *t)
{
    if (t->state == TASK_BLOCKED) {
        t->state = TASK_READY;
    }
    t->woken = 1;
}

void wake_signalled(struct task *t)
{
    u16 sr = irq_save();

    if (t->queue) {
        dequeue(t->queue, t);
    }
    wake_task(t);
    irq_restore(sr);
}

void wake_one(struct waitq *q)
{
    u16 sr = irq_save();
    struct task *t = q->head;

    if (t) {
        q->head = t->wait_next;
        t->wait_next = 0;
        t->queue = 0;
        wake_task(t);
    }
    irq_restore(sr);
}

void wake_all(struct waitq *q)
{
    u16 sr = irq_save();
    struct task *t = q->head;

    q->head = 0;
    while (t) {
        struct task *next = t->wait_next;

        t->wait_next = 0;
        t->queue = 0;
        wake_task(t);
        t = next;
    }
    irq_restore(sr);
}

/* --- semaphores ------------------------------------------------------ */

void sem_init(struct semaphore *s, int count)
{
    s->count = count;
    s->wait.head = 0;
}

void sem_wait(struct semaphore *s)
{
    for (;;) {
        u16 sr = irq_save();

        if (s->count > 0) {
            s->count--;
            irq_restore(sr);
            return;
        }
        irq_restore(sr);
        sleep_on(&s->wait);
    }
}

int sem_trywait(struct semaphore *s)
{
    u16 sr = irq_save();
    int got = 0;

    if (s->count > 0) {
        s->count--;
        got = 1;
    }
    irq_restore(sr);
    return got;
}

void sem_post(struct semaphore *s)
{
    u16 sr = irq_save();

    s->count++;
    irq_restore(sr);

    /*
     * One, not all. A semaphore hands out one permit per post, so waking
     * everybody would have them all wake, find the count already taken,
     * and sleep again -- a thundering herd for no gain.
     */
    wake_one(&s->wait);
}

/* --- mutexes --------------------------------------------------------- */

void mutex_init(struct mutex *m)
{
    m->locked = 0;
    m->owner = 0;
    m->wait.head = 0;
}

void mutex_lock(struct mutex *m)
{
    for (;;) {
        u16 sr = irq_save();

        if (!m->locked) {
            m->locked = 1;
            m->owner = current;
            irq_restore(sr);
            return;
        }
        /*
         * Taking it twice is a deadlock against oneself, and it happens
         * when a function that locks is called from one that already
         * has. Saying so is better than hanging: the machine stops with
         * a reason instead of stopping.
         */
        if (m->owner == current) {
            irq_restore(sr);
            panic("mutex: locked twice by the same task");
        }
        irq_restore(sr);
        sleep_on(&m->wait);
    }
}

int mutex_trylock(struct mutex *m)
{
    u16 sr = irq_save();
    int got = 0;

    if (!m->locked) {
        m->locked = 1;
        m->owner = current;
        got = 1;
    }
    irq_restore(sr);
    return got;
}

void mutex_unlock(struct mutex *m)
{
    u16 sr = irq_save();

    m->locked = 0;
    m->owner = 0;
    irq_restore(sr);

    wake_one(&m->wait);
}
