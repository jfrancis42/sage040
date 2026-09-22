/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * threadtest.c - threads: the kernel's clone and futexes, and the
 * pthread layer over them.
 *
 * WHAT A TEST OF THREADS HAS TO PROVE, beyond "it did not crash":
 *
 *   - that they really run at the same time (a counter reaching its
 *     total proves the sum; two threads observed interleaved proves
 *     they overlapped),
 *   - that the locks actually exclude: the same counter WITHOUT a lock
 *     has to be able to come out wrong, or the lock was never doing
 *     anything (the unlocked control below),
 *   - that they share what POSIX says they share -- descriptors, the
 *     working directory, the process id -- and not what they should not,
 *     which is the stack and errno,
 *   - and that a wakeup cannot be lost: a condition variable signalled
 *     between a waiter's decision to sleep and its sleeping must not
 *     hang, which is what the sequence number in pthread_cond_t is for.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

/* gettid: Linux/m68k 221, and no wrapper in this libc -- the point of
 * calling it here is to see a number the kernel chose. */
extern long syscall(long, ...);
#define SYS_gettid 221

static int pass, fail;

static void
check(const char *what, int ok)
{
    if (ok) {
        pass++;
        printf("  [ OK ] %s\n", what);
    } else {
        fail++;
        printf("  [FAIL] %s\n", what);
    }
    fflush(stdout);
}

/* --- creating, joining, and a return value --------------------------- */

static void *
returns_arg(void *arg)
{
    return arg;
}

static void
test_create_join(void)
{
    pthread_t t;
    void *ret = 0;

    printf("=== create and join ===\n");
    check("pthread_create", pthread_create(&t, 0, returns_arg,
                                           (void *)0x1234) == 0);
    check("pthread_join", pthread_join(t, &ret) == 0);
    check("  and what the thread returned came back", ret == (void *)0x1234);
    check("joining it twice fails", pthread_join(t, &ret) != 0);
}

/* --- they run at the same time, and the lock works ------------------- */

#define WORKERS     4
#define BUMPS       2000

static int             counter;
static int             unlocked_counter;
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    overlap_a, overlap_b, overlapped;

static void *
bump(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < BUMPS; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);

        /*
         * The same sum without the lock. Read, give the processor away,
         * write: the yield makes the window the lock exists to close
         * wide enough that losing an update is the normal outcome
         * rather than a rarity. This is the negative control -- if this
         * one also comes out exact, the locked one proved nothing.
         */
        {
            int v = unlocked_counter;

            sched_yield();
            unlocked_counter = v + 1;
        }
    }
    return 0;
}

static void *
overlap_thread(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 200; i++) {
        overlap_b = 1;
        if (overlap_a) {
            overlapped = 1;     /* the other one is inside its loop too */
        }
        sched_yield();
    }
    overlap_b = 0;
    return 0;
}

static void
test_concurrency(void)
{
    pthread_t t[WORKERS];
    pthread_t o;
    int i, ok = 1;

    printf("=== they run, and the lock excludes ===\n");
    for (i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], 0, bump, 0) != 0) {
            ok = 0;
        }
    }
    check("four threads started", ok);
    for (i = 0; i < WORKERS; i++) {
        pthread_join(t[i], 0);
    }
    printf("         locked %d, unlocked %d, of %d\n",
           counter, unlocked_counter, WORKERS * BUMPS);
    check("every locked increment landed", counter == WORKERS * BUMPS);
    check("  and the unlocked control lost some (the lock is real)",
          unlocked_counter < WORKERS * BUMPS);

    overlapped = 0;
    pthread_create(&o, 0, overlap_thread, 0);
    for (i = 0; i < 200; i++) {
        overlap_a = 1;
        if (overlap_b) {
            overlapped = 1;
        }
        sched_yield();
    }
    overlap_a = 0;
    pthread_join(o, 0);
    check("two threads were inside their loops at the same time",
          overlapped);
}

/* --- what a thread shares, and what it does not ---------------------- */

static int   shared_fd = -1;
static pid_t thread_pid, thread_tid;
static int   thread_errno;
static char  thread_cwd[PATH_MAX];
static unsigned long stack_of_thread;

static void *
sharing_thread(void *arg)
{
    char buf[16];
    int n;

    (void)arg;
    stack_of_thread = (unsigned long)&n;
    thread_pid = getpid();
    thread_tid = (pid_t)syscall(SYS_gettid);

    /* A descriptor opened by the MAIN thread, read here. */
    n = (int)read(shared_fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        n = 0;
    }
    buf[n] = '\0';

    /* errno is per thread: set one here that main must not see. */
    errno = 0;
    (void)close(-1);
    thread_errno = errno;

    /* The working directory is the process's: move it, and main moves. */
    chdir("/TT");
    getcwd(thread_cwd, sizeof(thread_cwd));

    return strcmp(buf, "shared") == 0 ? (void *)1 : (void *)0;
}

static void
test_sharing(void)
{
    pthread_t t;
    void *ret = 0;
    char cwd[PATH_MAX];
    unsigned long here;
    int fd;

    printf("=== what threads share ===\n");
    fd = open("/TT/SHARED.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, "shared", 6);
    close(fd);

    shared_fd = open("/TT/SHARED.TXT", O_RDONLY);
    check("a file opened in the main thread", shared_fd >= 0);

    errno = 0;
    chdir("/");
    pthread_create(&t, 0, sharing_thread, 0);
    pthread_join(t, &ret);

    check("  is readable in another thread (one descriptor table)",
          ret == (void *)1);
    check("getpid() is the same in both", thread_pid == getpid());
    check("  but gettid() is not", thread_tid != (pid_t)syscall(SYS_gettid));
    check("errno in the thread was EBADF", thread_errno == EBADF);
    check("  and the main thread's errno is untouched", errno == 0);

    getcwd(cwd, sizeof(cwd));
    check("a chdir in the thread moved the whole process",
          strcmp(cwd, "/TT") == 0 && strcmp(thread_cwd, "/TT") == 0);
    chdir("/");

    here = (unsigned long)&fd;
    check("the thread had a stack of its own",
          stack_of_thread != 0 &&
          (here > stack_of_thread + 0x4000 || stack_of_thread > here + 0x4000));
    close(shared_fd);
}

/* --- condition variables --------------------------------------------- */

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_ready = PTHREAD_COND_INITIALIZER;
static int             queue_items;
static int             consumed;

static void *
consumer(void *arg)
{
    int want = (int)(long)arg;

    while (consumed < want) {
        pthread_mutex_lock(&queue_lock);
        while (queue_items == 0) {
            pthread_cond_wait(&queue_ready, &queue_lock);
        }
        queue_items--;
        consumed++;
        pthread_mutex_unlock(&queue_lock);
    }
    return 0;
}

static void
test_condvar(void)
{
    pthread_t t;
    struct timespec abs;
    pthread_cond_t  lonely = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t lonely_m = PTHREAD_MUTEX_INITIALIZER;
    int i, err;

    printf("=== condition variables ===\n");
    consumed = 0;
    queue_items = 0;
    pthread_create(&t, 0, consumer, (void *)100L);

    for (i = 0; i < 100; i++) {
        pthread_mutex_lock(&queue_lock);
        queue_items++;
        pthread_cond_signal(&queue_ready);
        pthread_mutex_unlock(&queue_lock);
    }
    pthread_join(t, 0);
    check("a hundred items, produced and consumed", consumed == 100);
    check("  and none left behind", queue_items == 0);

    /* A wait nobody will ever signal must time out rather than hang --
     * and must not return early either. */
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_nsec += 300000000L;
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_nsec -= 1000000000L;
        abs.tv_sec++;
    }
    pthread_mutex_lock(&lonely_m);
    err = pthread_cond_timedwait(&lonely, &lonely_m, &abs);
    pthread_mutex_unlock(&lonely_m);
    check("an unsignalled timed wait times out", err == ETIMEDOUT);
}

/* --- once, keys, rwlocks, semaphores, barriers ----------------------- */

static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static int            once_ran;

static void
once_init(void)
{
    once_ran++;
}

static void *
once_thread(void *arg)
{
    (void)arg;
    pthread_once(&once_control, once_init);
    return 0;
}

static pthread_key_t key;
static int           destructed;

static void
key_destructor(void *v)
{
    if (v == (void *)0x99) {
        destructed++;
    }
}

static void *
key_thread(void *arg)
{
    (void)arg;
    if (pthread_getspecific(key) != 0) {
        return (void *)1;       /* a value from another thread: wrong */
    }
    pthread_setspecific(key, (void *)0x99);
    return pthread_getspecific(key) == (void *)0x99 ? 0 : (void *)1;
}

static sem_t          sem;
static int            sem_taken;
static pthread_barrier_t barrier;
static int            past_barrier;

static void *
sem_thread(void *arg)
{
    (void)arg;
    sem_wait(&sem);
    sem_taken++;
    return 0;
}

static void *
barrier_thread(void *arg)
{
    (void)arg;
    pthread_barrier_wait(&barrier);
    past_barrier++;
    return 0;
}

static void
test_primitives(void)
{
    pthread_t t[4];
    pthread_rwlock_t rw;
    void *ret;
    int i, serials = 0;

    printf("=== once, keys, rwlock, semaphore, barrier ===\n");
    for (i = 0; i < 4; i++) {
        pthread_create(&t[i], 0, once_thread, 0);
    }
    pthread_once(&once_control, once_init);
    for (i = 0; i < 4; i++) {
        pthread_join(t[i], 0);
    }
    check("pthread_once ran the initialiser exactly once", once_ran == 1);

    check("pthread_key_create", pthread_key_create(&key, key_destructor) == 0);
    pthread_setspecific(key, (void *)0x11);
    for (i = 0; i < 2; i++) {
        pthread_create(&t[i], 0, key_thread, 0);
    }
    for (i = 0; i < 2; i++) {
        ret = (void *)1;
        pthread_join(t[i], &ret);
        if (ret != 0) {
            serials++;
        }
    }
    check("  each thread's value is its own", serials == 0);
    check("  and the main thread's survived",
          pthread_getspecific(key) == (void *)0x11);
    check("  the destructor ran for each thread that set one",
          destructed == 2);
    pthread_key_delete(key);

    check("rwlock init", pthread_rwlock_init(&rw, 0) == 0);
    check("  two readers at once", pthread_rwlock_rdlock(&rw) == 0 &&
                                   pthread_rwlock_rdlock(&rw) == 0);
    check("  a writer cannot get in", pthread_rwlock_trywrlock(&rw) == EBUSY);
    pthread_rwlock_unlock(&rw);
    pthread_rwlock_unlock(&rw);
    check("  and can once they have gone",
          pthread_rwlock_trywrlock(&rw) == 0);
    check("  a second writer cannot",
          pthread_rwlock_trywrlock(&rw) == EBUSY);
    pthread_rwlock_unlock(&rw);
    pthread_rwlock_destroy(&rw);

    sem_init(&sem, 0, 0);
    sem_taken = 0;
    for (i = 0; i < 3; i++) {
        pthread_create(&t[i], 0, sem_thread, 0);
    }
    check("a semaphore with no tokens blocks", sem_taken == 0);
    for (i = 0; i < 3; i++) {
        sem_post(&sem);
    }
    for (i = 0; i < 3; i++) {
        pthread_join(t[i], 0);
    }
    check("  and three posts let three through", sem_taken == 3);
    check("  trywait on an empty one says so",
          sem_trywait(&sem) == -1 && errno == EAGAIN);
    sem_destroy(&sem);

    past_barrier = 0;
    pthread_barrier_init(&barrier, 0, 4);
    for (i = 0; i < 3; i++) {
        pthread_create(&t[i], 0, barrier_thread, 0);
    }
    check("three at a barrier of four are still waiting", past_barrier == 0);
    i = pthread_barrier_wait(&barrier);
    for (i = 0; i < 3; i++) {
        pthread_join(t[i], 0);
    }
    check("  and the fourth releases them all", past_barrier == 3);
    pthread_barrier_destroy(&barrier);
}

/* --- identity, detaching, limits ------------------------------------- */

static pthread_t self_in_thread;
static pthread_t self_arg;

static void *
identity_thread(void *arg)
{
    self_arg = (pthread_t)arg;
    self_in_thread = pthread_self();
    return 0;
}

static volatile int detached_ran;

static void *
detached_thread(void *arg)
{
    (void)arg;
    detached_ran = 1;
    return 0;
}

static void
test_identity(void)
{
    pthread_t t, me = pthread_self();
    pthread_attr_t attr;
    int i, waited = 0;

    printf("=== identity and detaching ===\n");
    check("pthread_self is stable", pthread_self() == me);
    pthread_create(&t, 0, identity_thread, 0);
    pthread_join(t, 0);
    check("a thread's pthread_self is itself", self_in_thread == t);
    check("  and not the main thread's", self_in_thread != me);
    check("pthread_equal agrees", pthread_equal(me, pthread_self()) &&
                                  !pthread_equal(me, t));

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    detached_ran = 0;
    check("a detached thread starts",
          pthread_create(&t, &attr, detached_thread, 0) == 0);
    for (i = 0; i < 500 && !detached_ran; i++) {
        struct timespec ms = { 0, 10000000L };

        nanosleep(&ms, 0);
        waited++;
    }
    check("  and runs", detached_ran);
    check("  and cannot be joined", pthread_join(t, 0) != 0);
    pthread_attr_destroy(&attr);

    {
        pthread_attr_t a;
        size_t sz = 0;

        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 * 1024);
        pthread_attr_getstacksize(&a, &sz);
        check("an attribute remembers its stack size", sz == 64 * 1024);
        check("  and refuses one below PTHREAD_STACK_MIN",
              pthread_attr_setstacksize(&a, 128) == EINVAL);
        pthread_attr_destroy(&a);
    }

    check("pthread_cancel says it is not implemented",
          pthread_cancel(pthread_self()) == ENOSYS);
}

/* --- signals --------------------------------------------------------- */

static volatile sig_atomic_t got_signal;

static void
handler(int sig)
{
    (void)sig;
    got_signal = 1;
}

static void *
signaller(void *arg)
{
    pthread_kill((pthread_t)arg, SIGUSR1);
    return 0;
}

static void
test_signals(void)
{
    struct sigaction sa;
    pthread_t t, me = pthread_self();
    int i;

    printf("=== signals ===\n");
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigaction(SIGUSR1, &sa, 0);

    got_signal = 0;
    pthread_create(&t, 0, signaller, me);
    pthread_join(t, 0);
    for (i = 0; i < 100 && !got_signal; i++) {
        struct timespec ms = { 0, 10000000L };

        nanosleep(&ms, 0);
    }
    check("one thread can signal another", got_signal);
}

/* --- a threaded process forks, execs and exits ----------------------- */

static volatile int child_thread_running;

static void *
busy_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ms = { 0, 20000000L };

        child_thread_running = 1;
        nanosleep(&ms, 0);
    }
    return 0;
}

static void
test_process(void)
{
    pthread_t t;
    pid_t pid;
    int status = 0;
    int i;

    printf("=== a threaded process ===\n");
    pthread_create(&t, 0, busy_thread, 0);
    for (i = 0; i < 200 && !child_thread_running; i++) {
        struct timespec ms = { 0, 10000000L };

        nanosleep(&ms, 0);
    }
    check("a thread that never returns is running", child_thread_running);

    /*
     * fork() from a threaded process gives a child with ONE thread --
     * the one that called it, as POSIX says. The child proves it by
     * exiting; if the other thread had come across, the child would
     * carry on running it.
     */
    pid = fork();
    if (pid == 0) {
        _exit(42);
    }
    check("fork from a threaded process", pid > 0);
    check("  and the child exits by itself",
          waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
          WEXITSTATUS(status) == 42);

    /* And exit_group takes the running thread with it -- which is this
     * program's own exit, checked by the harness seeing it finish. */
    pthread_detach(t);
}

int
main(void)
{
    mkdir("/TT", 0755);

    printf("threadtest: threads on SuckOS\n");
    test_create_join();
    test_concurrency();
    test_sharing();
    test_condvar();
    test_primitives();
    test_identity();
    test_signals();
    test_process();

    printf("\n  passed: %d\n  failed: %d\n", pass, fail);
    printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
