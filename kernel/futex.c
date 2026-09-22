/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * futex.c - the sleep and the wake a thread library is built out of.
 *
 * See futex.h for what a futex is and why the check and the sleep have
 * to be one operation. This file is the table of addresses somebody is
 * asleep on.
 *
 * THE TABLE IS SMALL ON PURPOSE. An entry exists only while at least one
 * task is waiting on that address: a mutex nobody is contending for, a
 * condition variable nobody is waiting on, and a semaphore with tokens
 * left all appear here not at all. So the number of entries needed is
 * bounded by the number of tasks, not by the number of locks a program
 * has -- and a program with ten thousand mutexes needs one entry while
 * one thread blocks on one of them.
 */
#include "futex.h"
#include "task.h"
#include "wait.h"
#include "uaccess.h"
#include "errno.h"
#include "timer.h"
#include "signal.h"
#include "string.h"

#define FUTEX_BUCKETS   32

struct futex_bucket {
    struct addrspace *as;
    u32   uaddr;
    int   waiters;              /* 0 means the entry is free           */
    struct waitq q;
};

static struct futex_bucket buckets[FUTEX_BUCKETS];

/* The entry for this address, or null. */
static struct futex_bucket *find(struct addrspace *as, u32 uaddr)
{
    int i;

    for (i = 0; i < FUTEX_BUCKETS; i++) {
        if (buckets[i].waiters > 0 && buckets[i].as == as &&
            buckets[i].uaddr == uaddr) {
            return &buckets[i];
        }
    }
    return 0;
}

/* The entry for this address, making one if there is a free slot. */
static struct futex_bucket *find_or_make(struct addrspace *as, u32 uaddr)
{
    struct futex_bucket *b = find(as, uaddr);
    int i;

    if (b) {
        return b;
    }
    for (i = 0; i < FUTEX_BUCKETS; i++) {
        if (buckets[i].waiters == 0) {
            buckets[i].as = as;
            buckets[i].uaddr = uaddr;
            buckets[i].q.head = 0;
            return &buckets[i];
        }
    }
    return 0;
}

int futex_wake_at(struct addrspace *as, u32 uaddr, int n)
{
    struct futex_bucket *b = find(as, uaddr);
    int woken = 0;

    if (!b) {
        return 0;
    }
    /*
     * wake_one takes the head of the queue. Waking is all this does:
     * the woken task decides for itself whether the thing it waited
     * for has happened, which is why every waiter is in a loop.
     */
    while (woken < n && b->q.head) {
        wake_one(&b->q);
        woken++;
    }
    return woken;
}

/*
 * The word this futex names, read out of user memory.
 *
 * A futex word is 32 bits and must be aligned: an unaligned one would
 * cross a page boundary and stop being a single object, and Linux
 * refuses it too.
 */
static int read_word(u32 uaddr, u32 *out)
{
    if (uaddr & 3) {
        return -EINVAL;
    }
    return copy_from_user(out, uaddr, sizeof(*out));
}

/* The timeout, in milliseconds, or 0 for "no timeout". */
static s32 read_timeout(u32 timeout_uva, u32 *ms)
{
    struct timespec ts;
    int err;

    *ms = 0;
    if (!timeout_uva) {
        return 0;
    }
    err = copy_from_user(&ts, timeout_uva, sizeof(ts));
    if (err < 0) {
        return err;
    }
    /* This system's timespec is unsigned (uapi.h), so a "negative"
     * duration arrives as an enormous one; the nanoseconds field is
     * what can be checked, and out-of-range is out-of-range. */
    if (ts.tv_nsec >= 1000000000U) {
        return -EINVAL;
    }
    /* Round up: a wait of half a tick must not return immediately. */
    *ms = ts.tv_sec * 1000U + (ts.tv_nsec + 999999U) / 1000000U;
    if (*ms == 0) {
        *ms = 1;
    }
    return 0;
}

static s32 futex_wait(u32 uaddr, u32 val, u32 timeout_uva)
{
    struct addrspace *as = uaccess_current();
    struct futex_bucket *b;
    u32 now, ms;
    s32 err;

    if (!as) {
        return -EFAULT;         /* a kernel task has no futexes */
    }
    err = read_timeout(timeout_uva, &ms);
    if (err < 0) {
        return err;
    }

    /*
     * The check and the sleep, with nothing in between: a task inside a
     * system call is not preempted, so no other thread can change the
     * word between this read and the sleep below. That is the whole
     * reason FUTEX_WAIT takes the expected value at all.
     */
    err = read_word(uaddr, &now);
    if (err < 0) {
        return err;
    }
    if (now != val) {
        return -EAGAIN;
    }
    if (signal_pending(current)) {
        return -EINTR;
    }

    b = find_or_make(as, uaddr);
    if (!b) {
        return -ENOMEM;
    }
    b->waiters++;
    if (ms) {
        int woken = sleep_on_timeout(&b->q, ms);

        b->waiters--;
        if (!woken) {
            return -ETIMEDOUT;
        }
    } else {
        sleep_on(&b->q);
        b->waiters--;
    }

    /*
     * A signal wakes a futex wait, as it does every other sleep here,
     * and -EINTR is what a thread library expects: pthread_mutex_lock
     * loops, and the C library restarts the wait.
     */
    if (signal_pending(current)) {
        return -EINTR;
    }
    return 0;
}

s32 futex_call(u32 uaddr, int op, u32 val, u32 timeout_uva, u32 uaddr2,
               u32 val3)
{
    struct addrspace *as = uaccess_current();
    int cmd = op & FUTEX_CMD_MASK;
    u32 word;
    s32 err;

    (void)val3;
    if (!as) {
        return -EFAULT;
    }
    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, val, timeout_uva);

    case FUTEX_WAIT_BITSET:
        /* The bitset selects which wakes apply; with the one bitset a
         * pthread library uses it is an ordinary wait. The timeout is
         * absolute here, which nothing on this system asks for -- so it
         * is refused rather than quietly treated as relative. */
        if (timeout_uva) {
            return -ENOSYS;
        }
        return futex_wait(uaddr, val, 0);

    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        if (uaddr & 3) {
            return -EINVAL;
        }
        if (uaccess_check(uaddr, sizeof(u32), 0) < 0) {
            return -EFAULT;
        }
        return futex_wake_at(as, uaddr, (int)val > 0 ? (int)val : 0);

    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        /*
         * Moving sleepers from one futex to another rather than waking
         * them is an optimisation -- it stops a broadcast turning every
         * waiter into a contender for the same mutex. Waking them
         * instead is CORRECT, since a futex waiter always rechecks its
         * condition, and this machine runs one task at a time anyway,
         * so the stampede it prevents cannot happen here.
         */
        if (cmd == FUTEX_CMP_REQUEUE) {
            err = read_word(uaddr, &word);
            if (err < 0) {
                return err;
            }
            if (word != val3) {
                return -EAGAIN;
            }
        }
        (void)uaddr2;
        return futex_wake_at(as, uaddr, 0x7fffffff);

    default:
        return -ENOSYS;
    }
}
