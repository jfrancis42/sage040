/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * futex.h - what a thread waits on.
 *
 * A futex is a word of ordinary user memory plus two kernel operations:
 * "sleep unless this word has changed" and "wake whoever is sleeping on
 * it". Everything a pthread mutex, condition variable, semaphore or
 * barrier does is built from those two, and the point is what does NOT
 * happen: an uncontended lock is a compare-and-set in user space and no
 * system call at all. The kernel is only involved when a thread would
 * otherwise spin.
 *
 * WHY THE CHECK AND THE SLEEP MUST BE ONE OPERATION. Between a thread
 * deciding "the lock is held, I shall sleep" and actually sleeping, the
 * holder may release it and wake nobody -- and the sleeper would then
 * wait for a wakeup that has already happened. FUTEX_WAIT therefore
 * takes the value the caller expects and refuses to sleep (-EAGAIN) if
 * the word no longer holds it. This kernel makes that trivially atomic:
 * a task inside a system call cannot be preempted, so the read and the
 * sleep happen with nothing in between.
 *
 * WHAT IDENTIFIES A FUTEX HERE: the address space and the user address.
 * Linux keys a private futex the same way and a shared one by the
 * physical page, because two processes can map one page at different
 * addresses. Nothing here shares memory between address spaces -- threads
 * share an address space entire -- so the pair is exact, and FUTEX_PRIVATE
 * changes nothing. A shared-memory mapping would be the day this needs
 * the physical address instead.
 */
#ifndef FUTEX_H
#define FUTEX_H

#include "kernel.h"

struct addrspace;

/*
 * The operations, Linux's numbers. Only WAIT and WAKE are real work;
 * REQUEUE and CMP_REQUEUE are what a condition variable's broadcast
 * uses to move sleepers to the mutex rather than stampede them, and are
 * implemented as a wake, which is correct but less efficient.
 */
#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET      10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_CMD_MASK        (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

/*
 * The system call, as Linux shapes it: uaddr is a user address, and
 * `timeout` a user pointer to a struct timespec or 0. Returns 0 or the
 * number woken, or a negative errno.
 */
s32 futex_call(u32 uaddr, int op, u32 val, u32 timeout_uva, u32 uaddr2,
               u32 val3);

/*
 * Wake up to `n` waiters on (as, uaddr) from inside the kernel. This is
 * how a thread's exit tells a join that it has finished: the exit path
 * has no user context left to make a system call with.
 */
int futex_wake_at(struct addrspace *as, u32 uaddr, int n);

#endif /* FUTEX_H */
