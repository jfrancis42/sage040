/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright © 2026 Jeff Francis
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

/*
 * atomic64.c - the 64-bit atomic operations, which this processor has no
 * instruction for.
 *
 * The 68040 has CAS and CAS2, and CAS goes up to 32 bits. So a compiler
 * asked for an atomic operation on a 64-bit object cannot emit one: it
 * emits a CALL to __atomic_load_8 and friends instead, and expects the
 * runtime to provide them. On Linux that runtime is libatomic, which is
 * not built for this bare-metal toolchain. This file is that runtime.
 *
 * HOW IT IS MADE ATOMIC. A table of locks, indexed by a hash of the
 * address -- the same thing libatomic does. Two objects that hash the
 * same share a lock and wait for each other, which costs time and never
 * correctness. The locks themselves are 32-bit, so CAS implements them.
 *
 * WHAT A WAITER DOES. It gives up its turn rather than spinning: this
 * machine has one processor, so a thread spinning on a lock is a thread
 * stopping its holder from running. The hold is a handful of
 * instructions, so the wait is rare and short.
 *
 * THE ONE HAZARD, stated plainly because it is real: a SIGNAL HANDLER
 * that performs a 64-bit atomic operation on an object whose lock the
 * interrupted thread is holding will wait for a lock that cannot be
 * released until the handler returns. libatomic has the same hazard on
 * every platform that lacks the instruction. Nothing here does 64-bit
 * atomics from a signal handler.
 */

#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LOCKS 16                /* a power of two: see lock_for() */

static volatile int locks[LOCKS];

static inline int
cas(volatile int *p, int expect, int val)
{
    int old = expect;

    __asm__ volatile ("cas.l %0,%2,%1"
                      : "+d"(old), "+m"(*p)
                      : "d"(val)
                      : "memory", "cc");
    return old;
}

/*
 * Which lock covers this address. The shift drops the bits that are
 * always the same for an aligned object, so neighbouring objects land
 * on different locks rather than all on one.
 */
static volatile int *
lock_for(const volatile void *addr)
{
    uintptr_t a = (uintptr_t)addr;

    return &locks[(a >> 3) & (LOCKS - 1)];
}

static void
lock(volatile int *l)
{
    while (cas(l, 0, 1) != 0) {
        sched_yield();
    }
}

static void
unlock(volatile int *l)
{
    *l = 0;
}

/* --- the 8-byte operations gcc calls --------------------------------- */

uint64_t
__atomic_load_8(const volatile void *ptr, int memorder)
{
    volatile int *l = lock_for(ptr);
    uint64_t v;

    (void)memorder;
    lock(l);
    v = *(const volatile uint64_t *)ptr;
    unlock(l);
    return v;
}

void
__atomic_store_8(volatile void *ptr, uint64_t val, int memorder)
{
    volatile int *l = lock_for(ptr);

    (void)memorder;
    lock(l);
    *(volatile uint64_t *)ptr = val;
    unlock(l);
}

uint64_t
__atomic_exchange_8(volatile void *ptr, uint64_t val, int memorder)
{
    volatile int *l = lock_for(ptr);
    uint64_t old;

    (void)memorder;
    lock(l);
    old = *(volatile uint64_t *)ptr;
    *(volatile uint64_t *)ptr = val;
    unlock(l);
    return old;
}

bool
__atomic_compare_exchange_8(volatile void *ptr, void *expected, uint64_t desired,
                            bool weak, int success, int failure)
{
    volatile int *l = lock_for(ptr);
    uint64_t old;
    bool ok;

    (void)weak;
    (void)success;
    (void)failure;
    lock(l);
    old = *(volatile uint64_t *)ptr;
    ok = (old == *(uint64_t *)expected);
    if (ok) {
        *(volatile uint64_t *)ptr = desired;
    } else {
        *(uint64_t *)expected = old;
    }
    unlock(l);
    return ok;
}

#define FETCH_OP(name, op)                                          \
    uint64_t                                                        \
    __atomic_fetch_##name##_8(volatile void *ptr, uint64_t val,     \
                              int memorder)                         \
    {                                                               \
        volatile int *l = lock_for(ptr);                            \
        uint64_t old;                                               \
                                                                    \
        (void)memorder;                                             \
        lock(l);                                                    \
        old = *(volatile uint64_t *)ptr;                            \
        *(volatile uint64_t *)ptr = op;                             \
        unlock(l);                                                  \
        return old;                                                 \
    }

FETCH_OP(add, old + val)
FETCH_OP(sub, old - val)
FETCH_OP(and, old & val)
FETCH_OP(or,  old | val)
FETCH_OP(xor, old ^ val)
FETCH_OP(nand, ~(old & val))

/* --- the generic forms, for objects of any size ---------------------- */

/*
 * gcc emits these for a type it has no sized call for -- a struct, or
 * anything larger than eight bytes. They take a size and pointers
 * rather than values.
 */
void
__atomic_load(size_t size, const volatile void *ptr, void *ret, int memorder)
{
    volatile int *l = lock_for(ptr);

    (void)memorder;
    lock(l);
    memcpy(ret, (const void *)ptr, size);
    unlock(l);
}

void
__atomic_store(size_t size, volatile void *ptr, void *val, int memorder)
{
    volatile int *l = lock_for(ptr);

    (void)memorder;
    lock(l);
    memcpy((void *)ptr, val, size);
    unlock(l);
}

void
__atomic_exchange(size_t size, volatile void *ptr, void *val, void *ret,
                  int memorder)
{
    volatile int *l = lock_for(ptr);

    (void)memorder;
    lock(l);
    memcpy(ret, (const void *)ptr, size);
    memcpy((void *)ptr, val, size);
    unlock(l);
}

bool
__atomic_compare_exchange(size_t size, volatile void *ptr, void *expected,
                          void *desired, int success, int failure)
{
    volatile int *l = lock_for(ptr);
    bool ok;

    (void)success;
    (void)failure;
    lock(l);
    ok = memcmp((const void *)ptr, expected, size) == 0;
    if (ok) {
        memcpy((void *)ptr, desired, size);
    } else {
        memcpy(expected, (const void *)ptr, size);
    }
    unlock(l);
    return ok;
}

/*
 * Is an operation of this size done without a lock? Up to four bytes,
 * yes -- CAS does it in one instruction. Eight, no, and saying so is
 * what lets a program choose a different algorithm.
 */
bool
__atomic_is_lock_free(size_t size, const volatile void *ptr)
{
    (void)ptr;
    return size <= 4;
}
