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
 * dl.c - <dlfcn.h>, and dl_iterate_phdr from <link.h>.
 *
 * All of the work is ld.so's, which stays in the process after start
 * and offers it through the table in __sage040_dl (sage040-dl.h). These
 * are the C library's side: one lock around every call, since ld.so's
 * tables are not safe to change from two threads at once, and the
 * error string dlerror hands back.
 *
 * A STATIC program has no ld.so, and so no dynamic loading: dlopen
 * fails and says so, as it does under musl. dlsym is answered, from
 * nothing -- there is no table of the program's symbols to look in.
 *
 * What ld.so does and does not do -- every library RTLD_GLOBAL, none
 * ever unloaded -- is written down in ldso/ld.c, at dlopen.
 */

#define _GNU_SOURCE

#include "sage040-dl.h"
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>

extern const struct dl_iface *__sage040_dl;

/* The public structures and ld.so's are the same memory. */
_Static_assert(sizeof(Dl_info) == sizeof(struct dl_info_), "Dl_info");
_Static_assert(sizeof(struct dl_phdr_info) == sizeof(struct dl_phdr_info_),
               "dl_phdr_info");
_Static_assert(offsetof(struct dl_phdr_info, dlpi_tls_modid) ==
               offsetof(struct dl_phdr_info_, dlpi_tls_modid),
               "dl_phdr_info layout");

/* Recursive: a library's constructor, run inside dlopen, may itself
 * call dlopen or reach a TLS variable of a library loaded late. */
static pthread_mutex_t dl_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/* Taken around every call into ld.so, __tls_get_addr's too (tls.c). */
void
__sage040_dl_lock(int take)
{
    if (take) {
        pthread_mutex_lock(&dl_mutex);
    } else {
        pthread_mutex_unlock(&dl_mutex);
    }
}

/* The last failure, until dlerror() reports it. Per process, where
 * POSIX allows per thread: a program that loads libraries from two
 * threads and reads the errors crosswise is not one this has met. */
static char err_buf[256];
static int err_pending;

static void
set_error(const char *e)
{
    size_t n = e ? strlen(e) : 0;

    if (n >= sizeof(err_buf)) {
        n = sizeof(err_buf) - 1;
    }
    memcpy(err_buf, e ? e : "", n);
    err_buf[n] = '\0';
    err_pending = 1;
}

void *
dlopen(const char *file, int mode)
{
    const char *e = 0;
    void *h;

    if (!__sage040_dl) {
        set_error("dlopen: not available in a statically linked program");
        return 0;
    }
    __sage040_dl_lock(1);
    h = __sage040_dl->dlopen(file, mode, &e);
    if (!h) {
        set_error(e ? e : "dlopen failed");
    }
    __sage040_dl_lock(0);
    return h;
}

void *
dlsym(void *restrict handle, const char *restrict name)
{
    const char *e = 0;
    void *p;

    if (!__sage040_dl) {
        set_error("dlsym: not available in a statically linked program");
        return 0;
    }
    __sage040_dl_lock(1);
    p = __sage040_dl->dlsym(handle, name, __builtin_return_address(0), &e);
    if (!p && e) {
        set_error(e);
    }
    __sage040_dl_lock(0);
    return p;
}

int
dlclose(void *handle)
{
    const char *e = 0;
    int r;

    if (!__sage040_dl) {
        set_error("dlclose: not available in a statically linked program");
        return -1;
    }
    __sage040_dl_lock(1);
    r = __sage040_dl->dlclose(handle, &e);
    if (r) {
        set_error(e);
    }
    __sage040_dl_lock(0);
    return r;
}

char *
dlerror(void)
{
    if (!err_pending) {
        return 0;
    }
    err_pending = 0;
    return err_buf;
}

int
dladdr(const void *addr, Dl_info *info)
{
    int r;

    if (!__sage040_dl) {
        return 0;
    }
    __sage040_dl_lock(1);
    r = __sage040_dl->dladdr(addr, (struct dl_info_ *)info);
    __sage040_dl_lock(0);
    return r;
}

int
dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data)
{
    int r;

    if (!__sage040_dl) {
        return 0;
    }
    /* Not locked: a callback may itself call dlopen or dlsym, and would
     * deadlock. The table only ever grows. */
    r = __sage040_dl->iterate_phdr(
        (int (*)(struct dl_phdr_info_ *, unsigned long, void *))cb, data);
    return r;
}
