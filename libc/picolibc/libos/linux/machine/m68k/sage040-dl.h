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
 * sage040-dl.h - what the dynamic linker and the C library share.
 *
 * ONE DEFINITION, INCLUDED BY BOTH: ldso/ld.c, which is freestanding,
 * and tls.c and dl.c here. Two copies of a structure that crosses from
 * one to the other is the libc-keeps-a-private-copy mistake this tree
 * has already made once (resolv.c's private struct netinfo). So only
 * plain C types, nothing from either side's headers.
 *
 * THREAD-LOCAL STORAGE, as Linux/m68k lays it out -- it has to be this
 * layout, because the linker computes offsets against it and writes
 * them into every program:
 *
 *                  tcb - 8   tcb                          tcb + size
 *     ... dtv, spare  |  program's block | libraries' blocks  |
 *                           ^
 *     thread pointer = tcb + 0x7000     (TLS variant I, as MIPS/PowerPC)
 *
 * binutils computes a local-exec offset as `address - tls_vma - 0x7000`
 * (elf32-m68k.c, tpoff_base), so the program's block starts exactly at
 * tp - 0x7000; a DTP-relative offset is `address - tls_vma - 0x8000`, so
 * __tls_get_addr returns the module's block + offset + 0x8000. The TCB
 * -- a pointer to the thread's DTV, and a spare word -- sits just below
 * the program's block, where glibc puts it too.
 *
 * The 68040 has no thread-pointer register: the kernel keeps it per
 * task, and __m68k_read_tp asks for it with get_thread_area.
 */
#ifndef SAGE040_DL_H
#define SAGE040_DL_H

#define DL_TLS_TP_OFFSET    0x7000
#define DL_TLS_DTV_OFFSET   0x8000

/*
 * The DTV: dtv[0] is the number of slots after it, and dtv[m] the
 * address of this thread's block for module m, or 0 until it is made.
 * A module is an object with a PT_TLS segment; its number is its place
 * in ld.so's table plus one, so the program is always 1. The DTV is as
 * big as that table and never has to grow.
 */
#define DL_MAX_OBJS         16
#define DL_DTV_SLOTS        (DL_MAX_OBJS + 1)

struct dl_tcbhead {
    unsigned long *dtv;
    unsigned long spare;
};

/*
 * The STATIC TLS: every module present when the program started, laid
 * out in one block per thread, the same for every thread. A thread's
 * block is `size` bytes from the TCB, aligned to `align`; module i's
 * part of it starts `offset` bytes in, and its first `filesz` bytes are
 * copied from `image` (.tdata) and the rest to `memsz` are zero (.tbss).
 */
#define DL_TLS_MAX_STATIC   DL_MAX_OBJS

struct dl_tls_module {
    unsigned long modid;
    unsigned long offset;
    unsigned long filesz;
    unsigned long memsz;
    const void *image;
};

struct dl_tls_layout {
    unsigned long size;
    unsigned long align;
    unsigned long n;
    struct dl_tls_module mod[DL_TLS_MAX_STATIC];
};

/* dladdr's answer; <dlfcn.h>'s Dl_info has the same four words. */
struct dl_info_ {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
};

/* dl_iterate_phdr's; <link.h>'s struct dl_phdr_info, glibc's layout. */
struct dl_phdr_info_ {
    unsigned long dlpi_addr;
    const char *dlpi_name;
    const void *dlpi_phdr;
    unsigned short dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    unsigned long dlpi_tls_modid;
    void *dlpi_tls_data;
};

/*
 * THE LOADER'S SERVICES, for a program it started. ld.so fills one of
 * these and writes its address into the C library's __sage040_dl before
 * any initialiser runs; a static program has none, and the C library
 * answers from its own copy of the layout instead.
 *
 * Every call that can fail returns 0 or -1 and leaves a message in
 * *err, a string of ld.so's that is good until the next call.
 * None is reentrant: dl.c holds one lock around every call.
 */
#define DL_IFACE_VERSION    1

struct dl_iface {
    unsigned long version;
    const struct dl_tls_layout *tls;
    /* This thread's block for a module loaded after start, made on
     * first use; and giving back all such blocks of a thread's DTV. */
    void *(*tls_block)(unsigned long *dtv, unsigned long modid);
    void (*tls_release)(unsigned long *dtv);
    void *(*dlopen)(const char *file, int mode, const char **err);
    void *(*dlsym)(void *handle, const char *name, const void *caller,
                   const char **err);
    int (*dlclose)(void *handle, const char **err);
    int (*dladdr)(const void *addr, struct dl_info_ *info);
    int (*iterate_phdr)(int (*cb)(struct dl_phdr_info_ *, unsigned long,
                                  void *),
                        void *data);
};

/* The C library's own, between tls.c, dl.c, pthread.c and crt0.s. */
void *__sage040_tls_alloc(void);
void __sage040_tls_free(void *tcb);
void __libc_init_tls(void);
void __sage040_dl_lock(int take);

#endif /* SAGE040_DL_H */
