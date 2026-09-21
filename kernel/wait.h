/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * wait.h - waiting for something, without spinning.
 *
 * Until there was a scheduler, every wait in this system was a spin: the
 * terminal's read went round a loop asking the UART whether a character
 * had arrived, and the network's did the same to the card. That worked
 * because there was nothing else for the processor to do. There is now,
 * and so every one of those loops becomes a sleep on one of these.
 *
 * THE RACE THIS EXISTS TO AVOID. A driver's interrupt can wake a queue
 * between a task deciding to wait and the task actually sleeping, and if
 * the wakeup lands in that gap it is lost and the task sleeps forever.
 * The window is closed by masking interrupts across the decision and the
 * sleep together -- sleep_on() raises the mask, puts the task on the
 * queue, and only lowers it inside schedule() once the task is safely no
 * longer running. A caller writes:
 *
 *      while (!condition) {
 *          sleep_on(&queue);
 *      }
 *
 * and the loop is not optional: a wakeup means "look again", never "it
 * is your turn". More than one task may be woken for one event.
 *
 * SEMAPHORES AND MUTEXES are both built out of that, and a semaphore is
 * the more basic of the two -- a mutex is a binary semaphore that
 * remembers who holds it. The owner only begins to matter when something
 * cares about priority, which nothing here does yet, so it is recorded
 * for the sake of catching a double acquire rather than used.
 */
#ifndef WAIT_H
#define WAIT_H

#include "kernel.h"

struct task;
struct waitq;

/*
 * A queue is a pointer. Tasks are linked through their own wait_next
 * field, so a queue costs four bytes and no allocation -- which matters
 * because they end up embedded in drivers, sockets and pipes, and one
 * that needed memory could fail.
 */
struct waitq {
    struct task *head;
};

/*
 * Block the current task until somebody wakes this queue.
 *
 * Returns when it has been woken AND scheduled again. Call it in a loop
 * around the condition that is actually being waited for.
 */
void sleep_on(struct waitq *q);

/*
 * Block, but no longer than `ms`. Returns 1 if woken, 0 if the time ran
 * out. The timeout is what lets a driver waiting on hardware that has
 * stopped answering report a failure instead of hanging the machine.
 */
int  sleep_on_timeout(struct waitq *q, u32 ms);

/*
 * Wake one, or all.
 *
 * SAFE FROM AN INTERRUPT HANDLER, which is the point of them: a driver's
 * ISR calls wake_all() and returns, and the task it woke runs at the
 * next scheduling point rather than inside the interrupt.
 */
void wake_one(struct waitq *q);

/*
 * Wake exactly `t`, from whatever queue it is on, for a signal.
 *
 * Not wake_all(t->queue): a queue is often shared -- every task in
 * nanosleep() waits on the same one -- and waking all of it for one
 * task's signal cut every other sleeper's sleep short, silently, with
 * a return of 0.
 */
struct task;
void wake_signalled(struct task *t);
void wake_all(struct waitq *q);

/* --- semaphores ------------------------------------------------------ */

struct semaphore {
    int count;
    struct waitq wait;
};

void sem_init(struct semaphore *s, int count);

/* Take one. Blocks while the count is zero. */
void sem_wait(struct semaphore *s);

/* Take one if it can be done without blocking. 1 if it was taken. */
int  sem_trywait(struct semaphore *s);

/* Give one back. Safe from an interrupt handler -- which is the usual
 * case: the driver's ISR is what says the transfer finished. */
void sem_post(struct semaphore *s);

/* --- mutexes --------------------------------------------------------- */

struct mutex {
    int locked;
    struct task *owner;
    struct waitq wait;
};

void mutex_init(struct mutex *m);

/*
 * NOT SAFE FROM AN INTERRUPT HANDLER, and there is no version that is.
 * An interrupt cannot block, so it cannot wait for a lock; a handler
 * that needs to exclude a task has to mask interrupts instead, which is
 * what irq_save/irq_restore are for. A mutex here always means "between
 * tasks".
 */
void mutex_lock(struct mutex *m);
int  mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);

/* --- masking --------------------------------------------------------- */

/*
 * Raise the interrupt mask, and put it back.
 *
 * The kernel's only other exclusion mechanism, and the right one when
 * what has to be excluded is an interrupt handler rather than another
 * task. Keep the region short: nothing else on the machine runs inside
 * it, including the clock.
 */
u16  irq_save(void);
void irq_restore(u16 sr);

#endif /* WAIT_H */
