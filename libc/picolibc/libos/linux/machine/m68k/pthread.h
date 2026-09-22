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

/*
 * pthread.h - threads.
 *
 * A thread here is a TASK that shares its address space, descriptors and
 * working directory with the thread that made it: clone(CLONE_VM|...),
 * which is exactly what Linux does. Everything that waits -- a mutex, a
 * condition variable, a semaphore, a join -- waits on a futex, so an
 * uncontended lock is a compare-and-set and no system call at all.
 *
 * WHAT IS DIFFERENT FROM A BIG UNIX, and worth knowing before porting
 * something:
 *
 *   - There is no thread register and no compiler thread-local storage.
 *     __thr does not work. pthread_getspecific and friends do, and
 *     pthread_self() finds a thread by looking up its stack pointer, so
 *     the cost is a short search rather than a register read.
 *   - pthread_cancel is not implemented. Cancellation means unwinding a
 *     thread from wherever it happens to be, which needs cancellation
 *     points throughout the C library; a program should ask its threads
 *     to stop instead. It returns ENOSYS rather than pretending.
 *   - Scheduling attributes are accepted and ignored: this system has
 *     one scheduling policy (see nice(2)) and no realtime priorities.
 *   - A process may have at most PTHREAD_THREADS_MAX threads, and that
 *     is the same table the kernel's tasks come from.
 */
#ifndef _PTHREAD_H_
#define _PTHREAD_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <sched.h>
#include <signal.h>

_BEGIN_STD_C

#define PTHREAD_THREADS_MAX     64
#define PTHREAD_KEYS_MAX        64
#define PTHREAD_STACK_MIN       16384

/* Attributes. Small and by value: a program may keep one on its stack. */
typedef struct {
    int    detachstate;
    size_t stacksize;
    void  *stackaddr;
    int    inheritsched;
    int    schedpolicy;
    int    scope;
    int    guardsize;
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_INHERIT_SCHED   0
#define PTHREAD_EXPLICIT_SCHED  1
#define PTHREAD_SCOPE_SYSTEM    0
#define PTHREAD_SCOPE_PROCESS   1

struct __pthread;
typedef struct __pthread *pthread_t;

/*
 * A mutex is one word of user memory. 0 is free, 1 is held, and 2 is
 * held with somebody waiting -- the third state is what lets an
 * uncontended unlock skip the system call, because only a 2 has anyone
 * to wake.
 */
typedef struct {
    volatile int lock;
    int          type;
    int          count;         /* recursive: how many times held      */
    pthread_t    owner;
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_NORMAL        0
#define PTHREAD_MUTEX_RECURSIVE     1
#define PTHREAD_MUTEX_ERRORCHECK    2
#define PTHREAD_MUTEX_DEFAULT       PTHREAD_MUTEX_NORMAL

#define PTHREAD_MUTEX_INITIALIZER   { 0, PTHREAD_MUTEX_NORMAL, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP \
                                    { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP \
                                    { 0, PTHREAD_MUTEX_ERRORCHECK, 0, 0 }

/*
 * A condition variable is a sequence number. A waiter reads it, drops
 * the mutex and sleeps unless it has changed; a signal bumps it and
 * wakes. The number is what closes the window between the two, and is
 * why a wakeup can never be missed by a waiter that had already decided
 * to sleep.
 */
typedef struct {
    volatile int seq;
    int          waiters;
    int          clock;
} pthread_cond_t;

typedef struct {
    int clock;
} pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER    { 0, 0, 0 }

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  readers;
    pthread_cond_t  writers;
    int             nreaders;   /* readers holding it                  */
    int             writer;     /* a writer holds it                   */
    int             waiting_writers;
} pthread_rwlock_t;

typedef struct {
    int unused;
} pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER \
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, \
      PTHREAD_COND_INITIALIZER, 0, 0, 0 }

typedef struct {
    volatile int state;
} pthread_once_t;

#define PTHREAD_ONCE_INIT           { 0 }

typedef int pthread_key_t;

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    unsigned        count;      /* how many must arrive                */
    unsigned        waiting;
    unsigned        cycle;
} pthread_barrier_t;

typedef struct {
    int unused;
} pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD   (-1)

/*
 * A spinlock. This machine has one processor, so a thread that spins is
 * a thread stopping everything it is waiting for from running: it gives
 * up its turn rather than spinning, which is what makes it correct here
 * and why it is no faster than a mutex.
 */
typedef struct {
    volatile int lock;
} pthread_spinlock_t;

#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

#define PTHREAD_CANCEL_ENABLE       0
#define PTHREAD_CANCEL_DISABLE      1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED            ((void *)-1)

int  pthread_create(pthread_t *__thr, const pthread_attr_t *__attr,
                    void *(*__start)(void *), void *__arg);
int  pthread_join(pthread_t __thr, void **__value);
int  pthread_detach(pthread_t __thr);
void pthread_exit(void *__value) __attribute__((__noreturn__));
pthread_t pthread_self(void);
int  pthread_equal(pthread_t __a, pthread_t __b);
int  pthread_kill(pthread_t __thr, int __sig);
int  pthread_sigmask(int __how, const sigset_t *__set, sigset_t *__old);
int  pthread_cancel(pthread_t __thr);
int  pthread_setcancelstate(int __state, int *__old);
int  pthread_setcanceltype(int __type, int *__old);
void pthread_testcancel(void);
int  pthread_getattr_np(pthread_t __thr, pthread_attr_t *__attr);
int  pthread_setname_np(pthread_t __thr, const char *__name);
int  pthread_getname_np(pthread_t __thr, char *__buf, size_t __len);

int  pthread_attr_init(pthread_attr_t *__attr);
int  pthread_attr_destroy(pthread_attr_t *__attr);
int  pthread_attr_setdetachstate(pthread_attr_t *__attr, int __state);
int  pthread_attr_getdetachstate(const pthread_attr_t *__attr, int *__state);
int  pthread_attr_setstacksize(pthread_attr_t *__attr, size_t __size);
int  pthread_attr_getstacksize(const pthread_attr_t *__attr, size_t *__size);
int  pthread_attr_setstack(pthread_attr_t *__attr, void *__addr, size_t __size);
int  pthread_attr_getstack(const pthread_attr_t *__attr, void **__addr,
                           size_t *__size);
int  pthread_attr_setguardsize(pthread_attr_t *__attr, size_t __size);
int  pthread_attr_getguardsize(const pthread_attr_t *__attr, size_t *__size);
int  pthread_attr_setscope(pthread_attr_t *__attr, int __scope);
int  pthread_attr_getscope(const pthread_attr_t *__attr, int *__scope);
int  pthread_attr_setinheritsched(pthread_attr_t *__attr, int __inherit);
int  pthread_attr_getinheritsched(const pthread_attr_t *__attr, int *__inherit);
int  pthread_attr_setschedpolicy(pthread_attr_t *__attr, int __policy);
int  pthread_attr_getschedpolicy(const pthread_attr_t *__attr, int *__policy);
int  pthread_attr_setschedparam(pthread_attr_t *__attr,
                                const struct sched_param *__param);
int  pthread_attr_getschedparam(const pthread_attr_t *__attr,
                                struct sched_param *__param);

int  pthread_mutex_init(pthread_mutex_t *__m, const pthread_mutexattr_t *__a);
int  pthread_mutex_destroy(pthread_mutex_t *__m);
int  pthread_mutex_lock(pthread_mutex_t *__m);
int  pthread_mutex_trylock(pthread_mutex_t *__m);
int  pthread_mutex_timedlock(pthread_mutex_t *__m,
                             const struct timespec *__abstime);
int  pthread_mutex_unlock(pthread_mutex_t *__m);

int  pthread_mutexattr_init(pthread_mutexattr_t *__a);
int  pthread_mutexattr_destroy(pthread_mutexattr_t *__a);
int  pthread_mutexattr_settype(pthread_mutexattr_t *__a, int __type);
int  pthread_mutexattr_gettype(const pthread_mutexattr_t *__a, int *__type);
int  pthread_mutexattr_setpshared(pthread_mutexattr_t *__a, int __pshared);
int  pthread_mutexattr_getpshared(const pthread_mutexattr_t *__a,
                                  int *__pshared);

int  pthread_cond_init(pthread_cond_t *__c, const pthread_condattr_t *__a);
int  pthread_cond_destroy(pthread_cond_t *__c);
int  pthread_cond_wait(pthread_cond_t *__c, pthread_mutex_t *__m);
int  pthread_cond_timedwait(pthread_cond_t *__c, pthread_mutex_t *__m,
                            const struct timespec *__abstime);
int  pthread_cond_signal(pthread_cond_t *__c);
int  pthread_cond_broadcast(pthread_cond_t *__c);

int  pthread_condattr_init(pthread_condattr_t *__a);
int  pthread_condattr_destroy(pthread_condattr_t *__a);
int  pthread_condattr_setclock(pthread_condattr_t *__a, clockid_t __clock);
int  pthread_condattr_getclock(const pthread_condattr_t *__a,
                               clockid_t *__clock);
int  pthread_condattr_setpshared(pthread_condattr_t *__a, int __pshared);
int  pthread_condattr_getpshared(const pthread_condattr_t *__a, int *__pshared);

int  pthread_rwlock_init(pthread_rwlock_t *__l, const pthread_rwlockattr_t *__a);
int  pthread_rwlock_destroy(pthread_rwlock_t *__l);
int  pthread_rwlock_rdlock(pthread_rwlock_t *__l);
int  pthread_rwlock_tryrdlock(pthread_rwlock_t *__l);
int  pthread_rwlock_wrlock(pthread_rwlock_t *__l);
int  pthread_rwlock_trywrlock(pthread_rwlock_t *__l);
int  pthread_rwlock_unlock(pthread_rwlock_t *__l);

int  pthread_once(pthread_once_t *__once, void (*__init)(void));

int  pthread_key_create(pthread_key_t *__key, void (*__destructor)(void *));
int  pthread_key_delete(pthread_key_t __key);
int  pthread_setspecific(pthread_key_t __key, const void *__value);
void *pthread_getspecific(pthread_key_t __key);

int  pthread_barrier_init(pthread_barrier_t *__b,
                          const pthread_barrierattr_t *__a, unsigned __count);
int  pthread_barrier_destroy(pthread_barrier_t *__b);
int  pthread_barrier_wait(pthread_barrier_t *__b);

int  pthread_spin_init(pthread_spinlock_t *__s, int __pshared);
int  pthread_spin_destroy(pthread_spinlock_t *__s);
int  pthread_spin_lock(pthread_spinlock_t *__s);
int  pthread_spin_trylock(pthread_spinlock_t *__s);
int  pthread_spin_unlock(pthread_spinlock_t *__s);

int  pthread_getschedparam(pthread_t __thr, int *__policy,
                           struct sched_param *__param);
int  pthread_setschedparam(pthread_t __thr, int __policy,
                           const struct sched_param *__param);
int  pthread_getconcurrency(void);
int  pthread_setconcurrency(int __level);

/* In <unistd.h> as well, where picolibc declares it. */
int  pthread_atfork(void (*__prepare)(void), void (*__parent)(void),
                    void (*__child)(void));

_END_STD_C

#endif /* _PTHREAD_H_ */
