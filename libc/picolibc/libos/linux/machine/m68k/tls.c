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
 * tls.c - thread-local storage: each thread's block, and the lookup
 * behind __tls_get_addr.
 *
 * The layout is Linux/m68k's and is described in sage040-dl.h, which
 * ld.so includes too. A DYNAMIC program's layout -- the program's and
 * every start-time library's TLS -- is ld.so's, which also makes the
 * first thread's block before anything else runs, and hands the C
 * library its services in __sage040_dl. A STATIC program has no ld.so:
 * crt0 calls __libc_init_tls, which reads the one module there is from
 * symbols libc/sage040.ld defines, and makes the first block itself.
 *
 * After that the two are the same: pthread_create asks for a block
 * shaped by whichever layout there is (__sage040_tls_alloc), and the
 * kernel is given its address + 0x7000 as the new thread's pointer.
 */

#define _GNU_SOURCE

#include "../../local-linux.h"
#include "sage040-dl.h"
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Set by ld.so before any initialiser runs; null in a static program. */
const struct dl_iface *__sage040_dl;

/*
 * The static program's one module, from the linker script. Weak, so a
 * program linked without them -- anything not using sage040.ld -- still
 * links, and simply has no TLS.
 */
extern char __tls_template_start[] __attribute__((weak));
extern char __tls_template_filesz[] __attribute__((weak));
extern char __tls_template_memsz[] __attribute__((weak));
extern char __tls_template_align[] __attribute__((weak));

static struct dl_tls_layout static_layout;

static const struct dl_tls_layout *
layout(void)
{
    return __sage040_dl ? __sage040_dl->tls : &static_layout;
}

static unsigned long
align_up(unsigned long v, unsigned long a)
{
    return (v + a - 1) & ~(a - 1);
}

static unsigned long
map_size(const struct dl_tls_layout *l)
{
    unsigned long total = 8 + l->align + l->size + DL_DTV_SLOTS * 4 + 4;

    return align_up(total, 4096);
}

/*
 * A block for a new thread: TCB, static TLS copied from the images,
 * DTV. Returns the TCB's address -- the thread pointer less 0x7000 --
 * or 0 if there is no memory. The same shape ld.so gives the first
 * thread (ld.c, tls_first_thread); the mapping's own address is kept in
 * the TCB's spare word, for __sage040_tls_free.
 */
void *
__sage040_tls_alloc(void)
{
    const struct dl_tls_layout *l = layout();
    unsigned long raw, tcb, *dtv, i;
    void *m = mmap(0, map_size(l), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (m == MAP_FAILED) {
        return 0;
    }
    raw = (unsigned long)m;
    tcb = align_up(raw + 8, l->align);
    dtv = (unsigned long *)align_up(tcb + l->size, 4);
    dtv[0] = DL_DTV_SLOTS - 1;
    for (i = 0; i < l->n; i++) {
        const struct dl_tls_module *mod = &l->mod[i];

        memcpy((void *)(tcb + mod->offset), mod->image, mod->filesz);
        dtv[mod->modid] = tcb + mod->offset;    /* the rest is zero */
    }
    ((struct dl_tcbhead *)(tcb - 8))->dtv = dtv;
    ((struct dl_tcbhead *)(tcb - 8))->spare = raw;
    return (void *)tcb;
}

/* A finished thread's: what ld.so made for libraries loaded late, then
 * the block itself. */
void
__sage040_tls_free(void *tcbp)
{
    struct dl_tcbhead *h;

    if (!tcbp) {
        return;
    }
    h = (struct dl_tcbhead *)((char *)tcbp - 8);
    if (__sage040_dl) {
        __sage040_dl->tls_release(h->dtv);
    }
    munmap((void *)h->spare, map_size(layout()));
}

/*
 * A static program's TLS, before its constructors run (crt0.s). ld.so
 * has done all of this for a dynamic program, so it is not called there.
 */
void
__libc_init_tls(void)
{
    void *tcb;

    if (__tls_template_memsz && (unsigned long)__tls_template_memsz) {
        struct dl_tls_module *m = &static_layout.mod[0];

        m->modid = 1;
        m->offset = 0;
        m->filesz = (unsigned long)__tls_template_filesz;
        m->memsz = (unsigned long)__tls_template_memsz;
        m->image = __tls_template_start;
        static_layout.n = 1;
        static_layout.size = m->memsz;
        static_layout.align = (unsigned long)__tls_template_align;
    }
    if (static_layout.align < 4) {
        static_layout.align = 4;
    }
    tcb = __sage040_tls_alloc();
    if (tcb) {
        syscall(LINUX_SYS_set_thread_area,
                (unsigned long)tcb + DL_TLS_TP_OFFSET);
    }
}

/*
 * The general-dynamic access, reached from __tls_get_addr (read_tp.S).
 * The module's block is in this thread's DTV -- always, for a module
 * present at start. One loaded later by dlopen has its block made on
 * the first access from each thread, by ld.so. Nothing sensible can be
 * returned for a module there is no block for, so that ends the program
 * as a bad pointer would, but saying why.
 */
struct tls_index {
    unsigned long module;
    unsigned long offset;
};

extern void *__m68k_read_tp(void);
__attribute__((visibility("hidden"))) void *
__sage040_tls_get_addr(const struct tls_index *ti);

__attribute__((visibility("hidden"))) void *
__sage040_tls_get_addr(const struct tls_index *ti)
{
    unsigned long tp = (unsigned long)__m68k_read_tp();
    unsigned long *dtv =
        ((struct dl_tcbhead *)(tp - DL_TLS_TP_OFFSET - 8))->dtv;
    unsigned long b = ti->module < DL_DTV_SLOTS ? dtv[ti->module] : 0;

    if (!b && __sage040_dl) {
        __sage040_dl_lock(1);
        b = (unsigned long)__sage040_dl->tls_block(dtv, ti->module);
        __sage040_dl_lock(0);
    }
    if (!b) {
        static const char msg[] =
            "thread-local storage: no block for this module\n";

        write(2, msg, sizeof(msg) - 1);
        _exit(127);
    }
    return (void *)(b + ti->offset + DL_TLS_DTV_OFFSET);
}
