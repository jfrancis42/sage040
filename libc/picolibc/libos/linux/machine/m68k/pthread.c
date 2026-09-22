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
 * pthread.c - POSIX threads over clone() and futexes.
 *
 * THREE THINGS DECIDE THE SHAPE OF THIS FILE.
 *
 * 1. A THREAD IS A TASK. pthread_create is clone(CLONE_VM|CLONE_FS|
 *    CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD) onto a stack from mmap, and
 *    the kernel's scheduler does the rest. Nothing here schedules
 *    anything.
 *
 * 2. EVERYTHING THAT WAITS, WAITS ON A FUTEX. A mutex is one word: taking
 *    an uncontended one is a compare-and-swap and no system call, and only
 *    a thread that actually has to block enters the kernel. The 68020's
 *    CAS instruction is what makes that possible, and it is a single
 *    instruction, so it is also atomic against the timer interrupt --
 *    which is the only thing that can interrupt a thread here.
 *
 * 3. THERE IS NO THREAD REGISTER. m68k has none to spare and this system
 *    has no thread-local storage in the compiler, so pthread_self() finds
 *    the current thread by looking for the descriptor whose stack the
 *    stack pointer is standing in. A short search over at most 64 entries,
 *    and the reason __thread does not work while pthread_getspecific does.
 *    The main thread is the one that matches nothing.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: cancellation. Unwinding a thread
 * from wherever it happens to be needs cancellation points throughout the
 * C library and a way to run cleanup handlers at each of them; half of it
 * is worse than none, so pthread_cancel returns ENOSYS and a program is
 * expected to ask its threads to stop.
 */

#include "../../local-linux.h"
#include "../../local-sigaction.h"
#include <errno.h>
#include <limits.h>
#include "pthread.h"
#include <sched.h>
#include "semaphore.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* --- the futex calls ------------------------------------------------- */

#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_PRIVATE_FLAG    128

static int
futex_wait(volatile int *addr, int val, const struct timespec *rel)
{
    return (int)syscall(LINUX_SYS_futex, addr,
                        FUTEX_WAIT | FUTEX_PRIVATE_FLAG, val, rel, 0, 0);
}

static int
futex_wake(volatile int *addr, int n)
{
    return (int)syscall(LINUX_SYS_futex, addr,
                        FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, 0, 0, 0);
}

/*
 * Give up the rest of this turn. Declared by <sched.h> and implemented
 * nowhere until threads wanted it -- a spinlock on a machine with one
 * processor is built out of it.
 */
int
sched_yield(void)
{
    return (int)syscall(LINUX_SYS_sched_yield);
}

/* --- atomics --------------------------------------------------------- */

/*
 * CAS.L Dc,Du,<ea>: compare Dc with the word, store Du if they match,
 * and load the word into Dc if they do not. One instruction, so it is
 * atomic against the timer interrupt -- which, on a machine with one
 * processor and preemption only at the return to user mode, is the whole
 * of what these have to be atomic against.
 */
static inline int
atomic_cas(volatile int *p, int expect, int val)
{
    int old = expect;

    __asm__ volatile ("cas.l %0,%2,%1"
                      : "+d"(old), "+m"(*p)
                      : "d"(val)
                      : "memory", "cc");
    return old;
}

static inline int
atomic_swap(volatile int *p, int val)
{
    int old;

    do {
        old = *p;
    } while (atomic_cas(p, old, val) != old);
    return old;
}

static inline int
atomic_add(volatile int *p, int n)
{
    int old;

    do {
        old = *p;
    } while (atomic_cas(p, old, old + n) != old);
    return old;
}

/* --- errno, per thread ----------------------------------------------- */

/*
 * WHY THIS FUNCTION EXISTS. errno is per thread, and has to be: a thread
 * that fails a call must not be told about some other thread's failure.
 * With no thread-local storage in the compiler, the only way to say so
 * is picolibc's hook -- built with -Derrno-function=__errno_location,
 * every use of errno becomes a call here, and each thread gets the word
 * in its own descriptor.
 *
 * It is declared in the header picolibc generates, so the definition
 * here is what that declaration refers to.
 */
int *__errno_location(void);

/* --- thread descriptors ---------------------------------------------- */

/*
 * A static table, of exactly the size of the kernel's task table: a
 * thread IS a task, so a program cannot have more of them than that, and
 * a table that cannot fail to allocate is one fewer thing to get wrong
 * while a thread is being made.
 */
struct __pthread {
    volatile int   tid;         /* the kernel's; zeroed by it at exit   */
    volatile int   used;        /* this slot is somebody's              */
    int            detached;
    int            joining;     /* somebody is in pthread_join on it    */
    void        *(*start)(void *);
    void          *arg;
    void          *retval;
    void          *stack;       /* what to unmap; null if not ours      */
    size_t         stacksize;
    unsigned long  lo, hi;      /* its stack, for pthread_self()        */
    unsigned long  guard;
    int            err;         /* this thread's errno                 */
    void          *specific[PTHREAD_KEYS_MAX];
    char           name[16];
};

static struct __pthread threads[PTHREAD_THREADS_MAX];
static pthread_mutex_t  table_lock = PTHREAD_MUTEX_INITIALIZER;

/* The key table: a destructor, or null, and whether the key is taken. */
static void (*key_destructor[PTHREAD_KEYS_MAX])(void *);
static int   key_used[PTHREAD_KEYS_MAX];

#define DEFAULT_STACK   (128 * 1024)

static unsigned long
stack_pointer(void)
{
    unsigned long sp;

    __asm__ volatile ("move.l %%sp,%0" : "=d"(sp));
    return sp;
}

/*
 * The main thread's descriptor is slot 0, filled in the first time
 * anything asks. Its stack is the process stack, whose bounds nothing
 * tells us -- so it is not registered, and "no descriptor claims this
 * stack pointer" is what identifies it. That is also true of a thread
 * some other library created behind our back, which would be a bug
 * rather than a case to handle.
 */
static void
main_thread_init(void)
{
    if (!threads[0].used) {
        threads[0].tid = (int)syscall(LINUX_SYS_gettid);
        threads[0].lo = threads[0].hi = 0;
        threads[0].detached = 1;
        strcpy(threads[0].name, "main");
        threads[0].used = 1;
    }
}

pthread_t
pthread_self(void)
{
    unsigned long sp = stack_pointer();
    int i;

    for (i = 1; i < PTHREAD_THREADS_MAX; i++) {
        if (threads[i].used && sp >= threads[i].lo && sp < threads[i].hi) {
            return &threads[i];
        }
    }
    main_thread_init();
    return &threads[0];
}

int *
__errno_location(void)
{
    return &pthread_self()->err;
}

int
pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

/* --- mutexes --------------------------------------------------------- */

/*
 * Three states, and the third is the point: 0 free, 1 held, 2 held with
 * somebody waiting. Only a 2 has anyone to wake, so an uncontended
 * unlock makes no system call at all -- which is what a lock taken and
 * dropped a million times by one thread costs here.
 */
int
pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    if (!m) {
        return EINVAL;
    }
    m->lock = 0;
    m->count = 0;
    m->owner = 0;
    m->type = a ? a->type : PTHREAD_MUTEX_NORMAL;
    return 0;
}

int
pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (!m) {
        return EINVAL;
    }
    if (m->lock != 0) {
        return EBUSY;
    }
    return 0;
}

static int
mutex_lock_common(pthread_mutex_t *m, const struct timespec *abstime, int try)
{
    int c;

    if (!m) {
        return EINVAL;
    }
    if (m->type != PTHREAD_MUTEX_NORMAL && m->owner == pthread_self()) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            if (m->count == INT_MAX) {
                return EAGAIN;
            }
            m->count++;
            return 0;
        }
        return EDEADLK;         /* errorcheck */
    }

    c = atomic_cas(&m->lock, 0, 1);
    if (c != 0) {
        if (try) {
            return EBUSY;
        }
        if (c != 2) {
            c = atomic_swap(&m->lock, 2);
        }
        while (c != 0) {
            if (abstime) {
                struct timespec now, rel;

                clock_gettime(CLOCK_REALTIME, &now);
                rel.tv_sec = abstime->tv_sec - now.tv_sec;
                rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
                if (rel.tv_nsec < 0) {
                    rel.tv_nsec += 1000000000L;
                    rel.tv_sec--;
                }
                if (rel.tv_sec < 0) {
                    return ETIMEDOUT;
                }
                if (futex_wait(&m->lock, 2, &rel) < 0 && errno == ETIMEDOUT) {
                    return ETIMEDOUT;
                }
            } else {
                futex_wait(&m->lock, 2, 0);
            }
            c = atomic_swap(&m->lock, 2);
        }
    }
    m->owner = pthread_self();
    m->count = 1;
    return 0;
}

int
pthread_mutex_lock(pthread_mutex_t *m)
{
    return mutex_lock_common(m, 0, 0);
}

int
pthread_mutex_trylock(pthread_mutex_t *m)
{
    return mutex_lock_common(m, 0, 1);
}

int
pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime)
{
    return mutex_lock_common(m, abstime, 0);
}

int
pthread_mutex_unlock(pthread_mutex_t *m)
{
    int old;

    if (!m) {
        return EINVAL;
    }
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != pthread_self()) {
            return EPERM;
        }
        if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count > 0) {
            return 0;
        }
    }
    m->owner = 0;
    m->count = 0;
    old = atomic_swap(&m->lock, 0);
    if (old == 2) {
        futex_wake(&m->lock, 1);
    }
    return 0;
}

int
pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    if (!a) {
        return EINVAL;
    }
    a->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

int
pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
    return a ? 0 : EINVAL;
}

int
pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (!a || type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    a->type = type;
    return 0;
}

int
pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
    if (!a || !type) {
        return EINVAL;
    }
    *type = a->type;
    return 0;
}

/* One process, so shared and private are the same thing. */
int
pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int pshared)
{
    if (!a) {
        return EINVAL;
    }
    return pshared == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP;
}

int
pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *pshared)
{
    if (!a || !pshared) {
        return EINVAL;
    }
    *pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

/* --- condition variables --------------------------------------------- */

/*
 * The sequence number is what makes a wakeup impossible to miss: a
 * waiter reads it while it still holds the mutex, and only then drops
 * the mutex and sleeps *unless the number has changed*. A signal that
 * lands in between changes the number, and the sleep does not happen.
 */
int
pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    if (!c) {
        return EINVAL;
    }
    c->seq = 0;
    c->waiters = 0;
    c->clock = a ? a->clock : CLOCK_REALTIME;
    return 0;
}

int
pthread_cond_destroy(pthread_cond_t *c)
{
    return c ? 0 : EINVAL;
}

static int
cond_wait_common(pthread_cond_t *c, pthread_mutex_t *m,
                 const struct timespec *abstime)
{
    int seq, err = 0;

    if (!c || !m) {
        return EINVAL;
    }
    seq = c->seq;
    atomic_add(&c->waiters, 1);
    pthread_mutex_unlock(m);

    for (;;) {
        struct timespec rel;

        if (abstime) {
            struct timespec now;

            clock_gettime(c->clock ? c->clock : CLOCK_REALTIME, &now);
            rel.tv_sec = abstime->tv_sec - now.tv_sec;
            rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) {
                rel.tv_nsec += 1000000000L;
                rel.tv_sec--;
            }
            if (rel.tv_sec < 0) {
                err = ETIMEDOUT;
                break;
            }
        }
        if (futex_wait(&c->seq, seq, abstime ? &rel : 0) < 0) {
            if (errno == ETIMEDOUT) {
                err = ETIMEDOUT;
                break;
            }
            /* EAGAIN: the number changed, which is the wakeup. EINTR:
             * a signal, and POSIX says a condition wait returns from
             * one only having been signalled -- so look again. */
            if (errno == EAGAIN) {
                break;
            }
        } else {
            break;
        }
        if (c->seq != seq) {
            break;
        }
    }

    atomic_add(&c->waiters, -1);
    pthread_mutex_lock(m);
    return err;
}

int
pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return cond_wait_common(c, m, 0);
}

int
pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                       const struct timespec *abstime)
{
    return cond_wait_common(c, m, abstime);
}

int
pthread_cond_signal(pthread_cond_t *c)
{
    if (!c) {
        return EINVAL;
    }
    atomic_add(&c->seq, 1);
    if (c->waiters > 0) {
        futex_wake(&c->seq, 1);
    }
    return 0;
}

int
pthread_cond_broadcast(pthread_cond_t *c)
{
    if (!c) {
        return EINVAL;
    }
    atomic_add(&c->seq, 1);
    if (c->waiters > 0) {
        futex_wake(&c->seq, INT_MAX);
    }
    return 0;
}

int
pthread_condattr_init(pthread_condattr_t *a)
{
    if (!a) {
        return EINVAL;
    }
    a->clock = CLOCK_REALTIME;
    return 0;
}

int
pthread_condattr_destroy(pthread_condattr_t *a)
{
    return a ? 0 : EINVAL;
}

int
pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock)
{
    if (!a) {
        return EINVAL;
    }
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) {
        return EINVAL;
    }
    a->clock = (int)clock;
    return 0;
}

int
pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock)
{
    if (!a || !clock) {
        return EINVAL;
    }
    *clock = a->clock;
    return 0;
}

int
pthread_condattr_setpshared(pthread_condattr_t *a, int pshared)
{
    if (!a) {
        return EINVAL;
    }
    return pshared == PTHREAD_PROCESS_PRIVATE ? 0 : ENOTSUP;
}

int
pthread_condattr_getpshared(const pthread_condattr_t *a, int *pshared)
{
    if (!a || !pshared) {
        return EINVAL;
    }
    *pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

/* --- read/write locks ------------------------------------------------ */

int
pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a)
{
    (void)a;
    if (!l) {
        return EINVAL;
    }
    pthread_mutex_init(&l->m, 0);
    pthread_cond_init(&l->readers, 0);
    pthread_cond_init(&l->writers, 0);
    l->nreaders = 0;
    l->writer = 0;
    l->waiting_writers = 0;
    return 0;
}

int
pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    if (!l) {
        return EINVAL;
    }
    if (l->nreaders || l->writer) {
        return EBUSY;
    }
    return 0;
}

int
pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    if (!l) {
        return EINVAL;
    }
    pthread_mutex_lock(&l->m);
    /* A waiting writer goes first, or a stream of readers could keep
     * one out for ever. */
    while (l->writer || l->waiting_writers) {
        pthread_cond_wait(&l->readers, &l->m);
    }
    l->nreaders++;
    pthread_mutex_unlock(&l->m);
    return 0;
}

int
pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    int err = 0;

    if (!l) {
        return EINVAL;
    }
    pthread_mutex_lock(&l->m);
    if (l->writer || l->waiting_writers) {
        err = EBUSY;
    } else {
        l->nreaders++;
    }
    pthread_mutex_unlock(&l->m);
    return err;
}

int
pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    if (!l) {
        return EINVAL;
    }
    pthread_mutex_lock(&l->m);
    l->waiting_writers++;
    while (l->writer || l->nreaders > 0) {
        pthread_cond_wait(&l->writers, &l->m);
    }
    l->waiting_writers--;
    l->writer = 1;
    pthread_mutex_unlock(&l->m);
    return 0;
}

int
pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    int err = 0;

    if (!l) {
        return EINVAL;
    }
    pthread_mutex_lock(&l->m);
    if (l->writer || l->nreaders > 0) {
        err = EBUSY;
    } else {
        l->writer = 1;
    }
    pthread_mutex_unlock(&l->m);
    return err;
}

int
pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    if (!l) {
        return EINVAL;
    }
    pthread_mutex_lock(&l->m);
    if (l->writer) {
        l->writer = 0;
    } else if (l->nreaders > 0) {
        l->nreaders--;
    }
    if (l->nreaders == 0) {
        pthread_cond_signal(&l->writers);
    }
    if (!l->waiting_writers) {
        pthread_cond_broadcast(&l->readers);
    }
    pthread_mutex_unlock(&l->m);
    return 0;
}

/* --- once ------------------------------------------------------------ */

int
pthread_once(pthread_once_t *once, void (*init)(void))
{
    int state;

    if (!once || !init) {
        return EINVAL;
    }
    for (;;) {
        state = once->state;
        if (state == 2) {
            return 0;           /* already run */
        }
        if (state == 0 && atomic_cas(&once->state, 0, 1) == 0) {
            init();
            once->state = 2;
            futex_wake(&once->state, INT_MAX);
            return 0;
        }
        /* Somebody else is running it: wait for them to finish, rather
         * than returning before the thing is initialised. */
        futex_wait(&once->state, 1, 0);
    }
}

/* --- thread-specific data -------------------------------------------- */

int
pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    int i;

    if (!key) {
        return EINVAL;
    }
    pthread_mutex_lock(&table_lock);
    for (i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if (!key_used[i]) {
            key_used[i] = 1;
            key_destructor[i] = destructor;
            pthread_mutex_unlock(&table_lock);
            *key = i;
            return 0;
        }
    }
    pthread_mutex_unlock(&table_lock);
    return EAGAIN;
}

int
pthread_key_delete(pthread_key_t key)
{
    int i;

    if (key < 0 || key >= PTHREAD_KEYS_MAX || !key_used[key]) {
        return EINVAL;
    }
    pthread_mutex_lock(&table_lock);
    key_used[key] = 0;
    key_destructor[key] = 0;
    /* The value goes from every thread: a key created again must not
     * find what the last owner of that slot left behind. */
    for (i = 0; i < PTHREAD_THREADS_MAX; i++) {
        threads[i].specific[key] = 0;
    }
    pthread_mutex_unlock(&table_lock);
    return 0;
}

int
pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key < 0 || key >= PTHREAD_KEYS_MAX || !key_used[key]) {
        return EINVAL;
    }
    pthread_self()->specific[key] = (void *)value;
    return 0;
}

void *
pthread_getspecific(pthread_key_t key)
{
    if (key < 0 || key >= PTHREAD_KEYS_MAX) {
        return 0;
    }
    return pthread_self()->specific[key];
}

/*
 * POSIX: run each key's destructor for each non-null value, then clear
 * it, and go round again up to four times -- because a destructor may
 * set another key.
 */
static void
run_destructors(struct __pthread *t)
{
    int round, i;

    for (round = 0; round < 4; round++) {
        int any = 0;

        for (i = 0; i < PTHREAD_KEYS_MAX; i++) {
            void *v = t->specific[i];

            if (v && key_used[i] && key_destructor[i]) {
                t->specific[i] = 0;
                key_destructor[i](v);
                any = 1;
            } else if (v) {
                t->specific[i] = 0;
            }
        }
        if (!any) {
            return;
        }
    }
}

/* --- creating and ending --------------------------------------------- */

int
pthread_attr_init(pthread_attr_t *a)
{
    if (!a) {
        return EINVAL;
    }
    memset(a, 0, sizeof(*a));
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->stacksize = DEFAULT_STACK;
    a->schedpolicy = SCHED_OTHER;
    a->scope = PTHREAD_SCOPE_SYSTEM;
    return 0;
}

int
pthread_attr_destroy(pthread_attr_t *a)
{
    return a ? 0 : EINVAL;
}

int
pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
    if (!a || (state != PTHREAD_CREATE_JOINABLE &&
               state != PTHREAD_CREATE_DETACHED)) {
        return EINVAL;
    }
    a->detachstate = state;
    return 0;
}

int
pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
    if (!a || !state) {
        return EINVAL;
    }
    *state = a->detachstate;
    return 0;
}

int
pthread_attr_setstacksize(pthread_attr_t *a, size_t size)
{
    if (!a || size < PTHREAD_STACK_MIN) {
        return EINVAL;
    }
    a->stacksize = size;
    return 0;
}

int
pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size)
{
    if (!a || !size) {
        return EINVAL;
    }
    *size = a->stacksize ? a->stacksize : DEFAULT_STACK;
    return 0;
}

int
pthread_attr_setstack(pthread_attr_t *a, void *addr, size_t size)
{
    if (!a || !addr || size < PTHREAD_STACK_MIN) {
        return EINVAL;
    }
    a->stackaddr = addr;
    a->stacksize = size;
    return 0;
}

int
pthread_attr_getstack(const pthread_attr_t *a, void **addr, size_t *size)
{
    if (!a || !addr || !size) {
        return EINVAL;
    }
    *addr = a->stackaddr;
    *size = a->stacksize ? a->stacksize : DEFAULT_STACK;
    return 0;
}

int
pthread_attr_setguardsize(pthread_attr_t *a, size_t size)
{
    if (!a) {
        return EINVAL;
    }
    a->guardsize = (int)size;
    return 0;
}

int
pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size)
{
    if (!a || !size) {
        return EINVAL;
    }
    *size = (size_t)a->guardsize;
    return 0;
}

int
pthread_attr_setscope(pthread_attr_t *a, int scope)
{
    if (!a) {
        return EINVAL;
    }
    if (scope != PTHREAD_SCOPE_SYSTEM) {
        return ENOTSUP;
    }
    a->scope = scope;
    return 0;
}

int
pthread_attr_getscope(const pthread_attr_t *a, int *scope)
{
    if (!a || !scope) {
        return EINVAL;
    }
    *scope = PTHREAD_SCOPE_SYSTEM;
    return 0;
}

int
pthread_attr_setinheritsched(pthread_attr_t *a, int inherit)
{
    if (!a) {
        return EINVAL;
    }
    a->inheritsched = inherit;
    return 0;
}

int
pthread_attr_getinheritsched(const pthread_attr_t *a, int *inherit)
{
    if (!a || !inherit) {
        return EINVAL;
    }
    *inherit = a->inheritsched;
    return 0;
}

int
pthread_attr_setschedpolicy(pthread_attr_t *a, int policy)
{
    if (!a) {
        return EINVAL;
    }
    /* One policy, and nice(2) is how a program asks for less of the
     * processor. Accepting SCHED_FIFO would be a promise about latency
     * that nothing here keeps. */
    if (policy != SCHED_OTHER) {
        return ENOTSUP;
    }
    a->schedpolicy = policy;
    return 0;
}

int
pthread_attr_getschedpolicy(const pthread_attr_t *a, int *policy)
{
    if (!a || !policy) {
        return EINVAL;
    }
    *policy = SCHED_OTHER;
    return 0;
}

int
pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p)
{
    if (!a || !p) {
        return EINVAL;
    }
    return 0;                   /* accepted and ignored: see above */
}

int
pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p)
{
    if (!a || !p) {
        return EINVAL;
    }
    memset(p, 0, sizeof(*p));
    return 0;
}

extern int __clone(int (*fn)(void *), void *stack_top, int flags, void *arg,
                   int *ptid, int *ctid);

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_PARENT_SETTID  0x00100000

/* Free the stack of any detached thread that has finished. A detached
 * thread cannot free its own stack -- it is standing on it -- so the
 * next create does it, which is the only moment it matters. */
static void
reap_detached(void)
{
    int i;

    for (i = 1; i < PTHREAD_THREADS_MAX; i++) {
        struct __pthread *t = &threads[i];

        if (t->used && t->detached && t->tid == 0) {
            t->used = 0;
            if (t->stack) {
                munmap(t->stack, t->stacksize);
            }
            memset(t, 0, sizeof(*t));
        }
    }
}

static int
thread_start(void *arg)
{
    struct __pthread *t = arg;

    t->retval = t->start(t->arg);
    run_destructors(t);
    return 0;                   /* __clone's child exits with this */
}

int
pthread_create(pthread_t *thread, const pthread_attr_t *attr,
               void *(*start)(void *), void *arg)
{
    pthread_attr_t def;
    struct __pthread *t = 0;
    void *stack;
    size_t size;
    int i, tid, flags;

    if (!start) {
        return EINVAL;
    }
    if (!attr) {
        pthread_attr_init(&def);
        attr = &def;
    }
    size = attr->stacksize ? attr->stacksize : DEFAULT_STACK;
    size = (size + 4095) & ~(size_t)4095;

    main_thread_init();
    pthread_mutex_lock(&table_lock);
    reap_detached();
    for (i = 1; i < PTHREAD_THREADS_MAX; i++) {
        if (!threads[i].used) {
            t = &threads[i];
            break;
        }
    }
    if (!t) {
        pthread_mutex_unlock(&table_lock);
        return EAGAIN;
    }

    if (attr->stackaddr) {
        stack = attr->stackaddr;
        t->stack = 0;           /* not ours to unmap */
    } else {
        stack = mmap(0, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) {
            pthread_mutex_unlock(&table_lock);
            return EAGAIN;
        }
        t->stack = stack;
    }

    memset(t->specific, 0, sizeof(t->specific));
    t->start = start;
    t->arg = arg;
    t->retval = 0;
    t->stacksize = size;
    t->detached = (attr->detachstate == PTHREAD_CREATE_DETACHED);
    t->joining = 0;
    t->tid = -1;                /* running, until the kernel clears it */
    t->name[0] = '\0';

    /*
     * The bounds go in BEFORE the slot is marked used, because
     * pthread_self() reads them without the lock: a scan must never see
     * a slot that claims a stack it has not been given yet.
     */
    t->lo = (unsigned long)stack;
    t->hi = (unsigned long)stack + size;
    t->used = 1;

    flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
            CLONE_THREAD | CLONE_CHILD_CLEARTID | CLONE_PARENT_SETTID;

    /*
     * The stack the thread starts on is the TOP of the block, aligned:
     * m68k stacks grow down, and a misaligned one makes every long
     * access in that thread slower and some of them wrong.
     */
    tid = __clone(thread_start, (char *)stack + (size & ~(size_t)3),
                  flags, t, (int *)&t->tid, (int *)&t->tid);
    if (tid < 0) {
        int err = errno;

        t->used = 0;
        if (t->stack) {
            munmap(t->stack, size);
        }
        memset(t, 0, sizeof(*t));
        pthread_mutex_unlock(&table_lock);
        return err == EAGAIN ? EAGAIN : err;
    }

    pthread_mutex_unlock(&table_lock);
    if (thread) {
        *thread = t;
    }
    return 0;
}

int
pthread_join(pthread_t t, void **value)
{
    int tid;

    if (!t || !t->used) {
        return ESRCH;
    }
    if (t == pthread_self()) {
        return EDEADLK;
    }
    if (t->detached) {
        return EINVAL;
    }
    if (t->joining) {
        return EINVAL;          /* only one thread may join another */
    }
    t->joining = 1;

    /*
     * The kernel zeroes t->tid when the thread ends and wakes whatever
     * is asleep on that address -- clone's CLONE_CHILD_CLEARTID. So a
     * join is a futex wait on one word, and a thread that has already
     * finished is a word that is already zero.
     */
    while ((tid = t->tid) != 0) {
        futex_wait(&t->tid, tid, 0);
    }

    if (value) {
        *value = t->retval;
    }
    t->used = 0;
    if (t->stack) {
        munmap(t->stack, t->stacksize);
    }
    memset(t, 0, sizeof(*t));
    return 0;
}

int
pthread_detach(pthread_t t)
{
    if (!t || !t->used) {
        return ESRCH;
    }
    if (t->joining) {
        return EINVAL;
    }
    t->detached = 1;
    return 0;
}

void
pthread_exit(void *value)
{
    struct __pthread *t = pthread_self();

    t->retval = value;
    run_destructors(t);

    /*
     * exit(2), not exit(3): this ends THIS THREAD. The process carries
     * on, and its other threads with it, which is what POSIX says even
     * when the thread calling this is the first one. A C library exit()
     * would run the atexit handlers and take the whole process down.
     */
    syscall(LINUX_SYS_exit, 0);
    for (;;) {                  /* not reached */
    }
}

/* --- signals --------------------------------------------------------- */

int
pthread_kill(pthread_t t, int sig)
{
    int lsig;

    if (!t || !t->used) {
        return ESRCH;
    }
    if (sig < 0 || sig >= NSIG) {
        return EINVAL;
    }
    /*
     * picolibc's signal numbers are newlib's and the kernel's are
     * Linux's, and they do not agree: SIGUSR1 is 16 here and 10 there.
     * Every other path through this library converts, and this one has
     * to as well -- sending the number unconverted delivered SIGSTKFLT,
     * whose default action killed the program.
     */
    lsig = sig ? _signal_to_linux(sig) : 0;
    if (sig && lsig < 0) {
        return EINVAL;
    }
    if (syscall(LINUX_SYS_tgkill, getpid(), t->tid, lsig) < 0) {
        return errno;
    }
    return 0;
}

int
pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    /* The mask is per thread, which is the whole point of this call
     * existing beside sigprocmask -- and here sigprocmask is already
     * per task, so they are the same operation. */
    if (sigprocmask(how, set, old) < 0) {
        return errno;
    }
    return 0;
}

/* --- cancellation: not implemented, and says so ---------------------- */

int
pthread_cancel(pthread_t t)
{
    (void)t;
    return ENOSYS;
}

int
pthread_setcancelstate(int state, int *old)
{
    (void)state;
    if (old) {
        *old = PTHREAD_CANCEL_ENABLE;
    }
    return 0;
}

int
pthread_setcanceltype(int type, int *old)
{
    (void)type;
    if (old) {
        *old = PTHREAD_CANCEL_DEFERRED;
    }
    return 0;
}

void
pthread_testcancel(void)
{
}

/* --- odds and ends --------------------------------------------------- */

int
pthread_getattr_np(pthread_t t, pthread_attr_t *a)
{
    if (!t || !a || !t->used) {
        return ESRCH;
    }
    pthread_attr_init(a);
    a->stackaddr = (void *)t->lo;
    a->stacksize = t->stacksize;
    a->detachstate = t->detached ? PTHREAD_CREATE_DETACHED
                                 : PTHREAD_CREATE_JOINABLE;
    return 0;
}

int
pthread_setname_np(pthread_t t, const char *name)
{
    if (!t || !name) {
        return EINVAL;
    }
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
    return 0;
}

int
pthread_getname_np(pthread_t t, char *buf, size_t len)
{
    if (!t || !buf || len == 0) {
        return EINVAL;
    }
    strncpy(buf, t->name, len - 1);
    buf[len - 1] = '\0';
    return 0;
}

int
pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p)
{
    if (!t || !policy || !p) {
        return EINVAL;
    }
    *policy = SCHED_OTHER;
    memset(p, 0, sizeof(*p));
    return 0;
}

int
pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p)
{
    (void)p;
    if (!t) {
        return EINVAL;
    }
    return policy == SCHED_OTHER ? 0 : ENOTSUP;
}

int
pthread_getconcurrency(void)
{
    return 0;
}

int
pthread_setconcurrency(int level)
{
    (void)level;
    return 0;
}

/* --- barriers -------------------------------------------------------- */

int
pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a,
                     unsigned count)
{
    (void)a;
    if (!b || count == 0) {
        return EINVAL;
    }
    pthread_mutex_init(&b->m, 0);
    pthread_cond_init(&b->c, 0);
    b->count = count;
    b->waiting = 0;
    b->cycle = 0;
    return 0;
}

int
pthread_barrier_destroy(pthread_barrier_t *b)
{
    if (!b) {
        return EINVAL;
    }
    if (b->waiting) {
        return EBUSY;
    }
    return 0;
}

int
pthread_barrier_wait(pthread_barrier_t *b)
{
    unsigned cycle;

    if (!b) {
        return EINVAL;
    }
    pthread_mutex_lock(&b->m);
    cycle = b->cycle;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->cycle++;
        pthread_cond_broadcast(&b->c);
        pthread_mutex_unlock(&b->m);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (cycle == b->cycle) {
        pthread_cond_wait(&b->c, &b->m);
    }
    pthread_mutex_unlock(&b->m);
    return 0;
}

/* --- spinlocks ------------------------------------------------------- */

int
pthread_spin_init(pthread_spinlock_t *s, int pshared)
{
    (void)pshared;
    if (!s) {
        return EINVAL;
    }
    s->lock = 0;
    return 0;
}

int
pthread_spin_destroy(pthread_spinlock_t *s)
{
    return s ? 0 : EINVAL;
}

int
pthread_spin_lock(pthread_spinlock_t *s)
{
    if (!s) {
        return EINVAL;
    }
    /* One processor: a thread that spins is stopping the thread holding
     * the lock from running. So it gives up its turn instead, which
     * makes this a slower mutex rather than a faster one -- correct,
     * and the honest thing for a program that asked for a spinlock. */
    while (atomic_cas(&s->lock, 0, 1) != 0) {
        sched_yield();
    }
    return 0;
}

int
pthread_spin_trylock(pthread_spinlock_t *s)
{
    if (!s) {
        return EINVAL;
    }
    return atomic_cas(&s->lock, 0, 1) == 0 ? 0 : EBUSY;
}

int
pthread_spin_unlock(pthread_spinlock_t *s)
{
    if (!s) {
        return EINVAL;
    }
    s->lock = 0;
    return 0;
}

/* --- POSIX semaphores ------------------------------------------------ */

int
sem_init(sem_t *s, int pshared, unsigned value)
{
    (void)pshared;
    if (!s || value > (unsigned)INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    s->count = (int)value;
    s->waiters = 0;
    return 0;
}

int
sem_destroy(sem_t *s)
{
    if (!s) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int
sem_wait_common(sem_t *s, const struct timespec *abstime)
{
    if (!s) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        int c = s->count;

        if (c > 0) {
            if (atomic_cas(&s->count, c, c - 1) == c) {
                return 0;
            }
            continue;
        }
        atomic_add(&s->waiters, 1);
        if (abstime) {
            struct timespec now, rel;

            clock_gettime(CLOCK_REALTIME, &now);
            rel.tv_sec = abstime->tv_sec - now.tv_sec;
            rel.tv_nsec = abstime->tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) {
                rel.tv_nsec += 1000000000L;
                rel.tv_sec--;
            }
            if (rel.tv_sec < 0) {
                atomic_add(&s->waiters, -1);
                errno = ETIMEDOUT;
                return -1;
            }
            if (futex_wait(&s->count, 0, &rel) < 0 && errno == ETIMEDOUT) {
                atomic_add(&s->waiters, -1);
                return -1;
            }
        } else if (futex_wait(&s->count, 0, 0) < 0 && errno == EINTR) {
            atomic_add(&s->waiters, -1);
            return -1;          /* a signal: POSIX says EINTR */
        }
        atomic_add(&s->waiters, -1);
    }
}

int
sem_wait(sem_t *s)
{
    return sem_wait_common(s, 0);
}

int
sem_timedwait(sem_t *s, const struct timespec *abstime)
{
    return sem_wait_common(s, abstime);
}

int
sem_trywait(sem_t *s)
{
    if (!s) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        int c = s->count;

        if (c <= 0) {
            errno = EAGAIN;
            return -1;
        }
        if (atomic_cas(&s->count, c, c - 1) == c) {
            return 0;
        }
    }
}

int
sem_post(sem_t *s)
{
    if (!s) {
        errno = EINVAL;
        return -1;
    }
    atomic_add(&s->count, 1);
    if (s->waiters > 0) {
        futex_wake(&s->count, 1);
    }
    return 0;
}

int
sem_getvalue(sem_t *s, int *value)
{
    if (!s || !value) {
        errno = EINVAL;
        return -1;
    }
    *value = s->count;
    return 0;
}
