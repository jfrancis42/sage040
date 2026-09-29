/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tlstest - thread-local storage, and dlopen.
 *
 * Built twice. `tlstest` is static: its TLS is set up by crt0 from the
 * linker script's symbols, and dlopen must refuse. `tlstest.dyn` is
 * dynamic and linked against libtlsa.so, and dlopens libtlsb.so (which
 * needs libtlsc.so) and libtlsie.so, which must be refused.
 *
 * What makes a TLS check worth anything is that the answer could have
 * come out the same with a single shared variable. So every per-thread
 * check has a thread change its copy and then look again after the
 * others have changed theirs, a new thread must see the INITIAL value
 * rather than its creator's current one, and addresses are compared.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, checks;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    checks++;
    if (!ok) {
        failures++;
    }
}

/* --- the program's own ----------------------------------------------- */

__thread int tv = 42;
__thread int tz;
__thread double ta __attribute__((aligned(16))) = 1.5;
__thread char tbig[5000];

#define NTHREADS 4

static pthread_barrier_t bar;

struct result {
    int start_tv, start_tz, start_big;
    int kept;                   /* its value survived the others' writes */
    uintptr_t addr;
    int aligned;
#ifdef DYNAMIC
    int a_start, a_kept, a_agree;
    int b_start, b_kept, b_big_zero;
    int (*b_addr)(void);
#endif
};

static struct result res[NTHREADS];

#ifdef DYNAMIC
extern __thread int a_var;
extern int *a_addr(void);
extern int a_get(void);
static int *(*b_addr_fn)(void);
static char *(*b_big_fn)(void);
#endif

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg, i;
    struct result *r = &res[id];

    r->start_tv = tv;
    r->start_tz = tz;
    r->start_big = 1;
    for (i = 0; i < (int)sizeof(tbig); i++) {
        if (tbig[i]) {
            r->start_big = 0;
        }
    }
    r->addr = (uintptr_t)&tv;
    r->aligned = ((uintptr_t)&ta % 16) == 0 && ta == 1.5;
#ifdef DYNAMIC
    r->a_start = a_var;
    r->a_agree = (&a_var == a_addr());
    if (b_addr_fn) {
        char *bb = b_big_fn();

        r->b_start = *b_addr_fn();
        r->b_big_zero = 1;
        for (i = 0; i < 3000; i++) {
            if (bb[i]) {
                r->b_big_zero = 0;
            }
        }
        *b_addr_fn() = 1000 + id;
        memset(bb, id + 1, 3000);
    }
    a_var = 500 + id;
#endif
    tv = id * 10;
    memset(tbig, id + 1, sizeof(tbig));

    pthread_barrier_wait(&bar);     /* everybody has written theirs */
    sched_yield();
    pthread_barrier_wait(&bar);

    r->kept = tv == id * 10 && tbig[0] == id + 1 && tbig[4999] == id + 1;
#ifdef DYNAMIC
    r->a_kept = a_var == 500 + id && a_get() == 500 + id;
    if (b_addr_fn) {
        r->b_kept = *b_addr_fn() == 1000 + id && b_big_fn()[2999] == id + 1;
    }
#endif
    return 0;
}

static void threads(const char *label)
{
    pthread_t t[NTHREADS];
    int i, ok;
    char what[120];

    tv = 7;                         /* a new thread must not see this */
    memset(res, 0, sizeof(res));
    pthread_barrier_init(&bar, 0, NTHREADS);
    for (i = 0; i < NTHREADS; i++) {
        pthread_create(&t[i], 0, worker, (void *)(intptr_t)i);
    }
    for (i = 0; i < NTHREADS; i++) {
        pthread_join(t[i], 0);
    }

    for (ok = 1, i = 0; i < NTHREADS; i++) {
        ok = ok && res[i].start_tv == 42 && res[i].start_tz == 0 &&
             res[i].start_big;
    }
    snprintf(what, sizeof(what), "%s: each new thread starts from the "
             "initial values, not its creator's", label);
    check(what, ok);
    for (ok = 1, i = 0; i < NTHREADS; i++) {
        ok = ok && res[i].kept;
    }
    snprintf(what, sizeof(what), "%s: and keeps its own through the others' "
             "writes", label);
    check(what, ok);
    for (ok = 1, i = 0; i < NTHREADS; i++) {
        int j;

        for (j = 0; j < i; j++) {
            ok = ok && res[i].addr != res[j].addr;
        }
        ok = ok && res[i].addr != (uintptr_t)&tv;
    }
    snprintf(what, sizeof(what), "%s: at an address of its own", label);
    check(what, ok);
    for (ok = 1, i = 0; i < NTHREADS; i++) {
        ok = ok && res[i].aligned;
    }
    snprintf(what, sizeof(what), "%s: a 16-byte-aligned variable is "
             "aligned in every thread", label);
    check(what, ok);
    check("  and the first thread's value was not disturbed", tv == 7);
}

static void basics(void)
{
    int st;
    pid_t pid;

    check("initial values: .tdata copied, .tbss zero",
          tv == 42 && tz == 0 && ta == 1.5 && tbig[0] == 0 && tbig[4999] == 0);
    check("the first thread's 16-byte-aligned variable is aligned",
          ((uintptr_t)&ta % 16) == 0);
    tv = 99;
    tbig[100] = 'x';
    pid = fork();
    if (pid == 0) {
        _exit(tv == 99 && tbig[100] == 'x' ? 0 : 1);
    }
    waitpid(pid, &st, 0);
    check("a forked child has its parent's values", WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
}

#ifdef DYNAMIC
static int phdr_count, phdr_tls_a;

static int count_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    (void)data;
    phdr_count++;
    if (strstr(info->dlpi_name, "libtlsa.so") && info->dlpi_tls_modid &&
        info->dlpi_tls_data == (void *)((char *)a_addr() - 0)) {
        phdr_tls_a = 1;
    }
    return 0;
}

static void startlib(void)
{
    check("a library's TLS at start: its initial value",
          a_var == 100 && a_get() == 100);
    check("  the program's access and the library's agree on the address",
          &a_var == a_addr());
}

static void dynamic(void)
{
    void *h, *h2, *hc;
    const char *e;
    Dl_info di;
    int before, *bp;
    int (*calls_c)(void);
    int *ctor;

    /* Errors first, and dlerror's once-only rule. */
    h = dlopen("libnosuch.so", RTLD_NOW);
    e = dlerror();
    check("dlopen of a library that is not there: NULL, and dlerror says so",
          !h && e && strstr(e, "libnosuch.so"));
    check("  and dlerror is empty the second time", dlerror() == 0);

    phdr_count = 0;
    dl_iterate_phdr(count_cb, 0);
    before = phdr_count;
    h = dlopen("libtlsie.so", RTLD_NOW);
    e = dlerror();
    check("a library with initial-exec TLS is refused after start",
          !h && e && strstr(e, "initial-exec"));
    phdr_count = 0;
    dl_iterate_phdr(count_cb, 0);
    check("  and leaves nothing of itself behind", phdr_count == before);

    h = dlopen("libtlsb.so", RTLD_NOW);
    check("dlopen of a library, and of the library it needs", h != 0);
    if (!h) {
        printf("         (%s)\n", dlerror());
        return;
    }
    ctor = dlsym(h, "b_ctor_ran");
    check("  its constructor ran", ctor && *ctor == 1);
    calls_c = (int (*)(void))dlsym(h, "b_calls_c");
    check("  and its call into its own dependency works",
          calls_c && calls_c() == 77);
    hc = dlsym(h, "c_func");
    check("  dlsym on it finds its dependency's symbols too", hc != 0);
    check("  but not a symbol nobody has", dlsym(h, "nosuch") == 0 &&
          (e = dlerror()) && strstr(e, "nosuch"));
    h2 = dlopen("libtlsb.so", RTLD_NOW);
    check("  a second dlopen is the same handle", h2 == h);
    check("  dlclose answers 0", dlclose(h2) == 0);

    b_addr_fn = (int *(*)(void))dlsym(h, "b_addr");
    b_big_fn = (char *(*)(void))dlsym(h, "b_big_addr");
    bp = b_addr_fn ? b_addr_fn() : 0;
    check("a late library's TLS: its initial value, made on first use",
          bp && *bp == 200);
    check("  and dlsym of the TLS variable is this thread's copy",
          dlsym(h, "b_var") == (void *)bp);
    *bp = 201;

    /* The library's own address for a_get, through its handle: the
     * program's `a_get` is its PLT entry, which is the canonical address
     * of the function everywhere in the process, not where it is. */
    {
        void *ha = dlopen("libtlsa.so", RTLD_NOLOAD);
        void *real = ha ? dlsym(ha, "a_get") : 0;

        check("dladdr names a library's function and its object",
              real && dladdr((char *)real + 2, &di) && di.dli_sname &&
              strcmp(di.dli_sname, "a_get") == 0 &&
              di.dli_saddr == real &&
              strstr(di.dli_fname, "libtlsa.so") != 0);
    }
    check("dlsym(RTLD_DEFAULT) finds a library's function, at the address "
          "the program uses for it", dlsym(RTLD_DEFAULT, "a_get") == (void *)a_get);
    check("dlopen(NULL) is the program", dlopen(0, RTLD_NOW) != 0);

    phdr_count = 0;
    phdr_tls_a = 0;
    dl_iterate_phdr(count_cb, 0);
    check("dl_iterate_phdr: every object, and a library's TLS block",
          phdr_count == before + 2 && phdr_tls_a);
}
#endif

int main(void)
{
    basics();
#ifdef DYNAMIC
    startlib();
    dynamic();
    threads("threads");
    {
        int ok = 1, i;

        for (i = 0; i < NTHREADS; i++) {
            ok = ok && res[i].a_start == 100 && res[i].a_agree;
        }
        check("threads: a start-time library's TLS, fresh in each", ok);
        for (ok = 1, i = 0; i < NTHREADS; i++) {
            ok = ok && res[i].a_kept;
        }
        check("  and kept", ok);
        for (ok = 1, i = 0; i < NTHREADS; i++) {
            ok = ok && res[i].b_start == 200 && res[i].b_big_zero;
        }
        check("threads: a dlopened library's TLS, made fresh in each", ok);
        for (ok = 1, i = 0; i < NTHREADS; i++) {
            ok = ok && res[i].b_kept;
        }
        check("  and kept", ok);
        check("  and the first thread's copy is still its own",
              b_addr_fn && *b_addr_fn() == 201);
    }
#else
    threads("threads");
    check("dlopen in a static program: NULL, and dlerror says why",
          dlopen("libtlsb.so", RTLD_NOW) == 0 && dlerror() != 0);
#endif
    printf("tlstest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
