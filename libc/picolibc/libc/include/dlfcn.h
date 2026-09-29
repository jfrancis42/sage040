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
 * <dlfcn.h>: loading a shared library while a program runs. The work is
 * the dynamic linker's (ldso/ld.c); the calls are in the C library
 * (libos/linux/machine/m68k/dl.c). Linux's values, so that code written
 * for Linux passes the flags it passes there.
 *
 * Every library is loaded RTLD_GLOBAL and RTLD_NODELETE, whatever is
 * asked: see ld.c.
 */
#ifndef _DLFCN_H_
#define _DLFCN_H_

#include <sys/cdefs.h>

#define RTLD_LAZY       0x00001 /* taken as RTLD_NOW: binding is eager */
#define RTLD_NOW        0x00002
#define RTLD_NOLOAD     0x00004
#define RTLD_DEEPBIND   0x00008 /* accepted, and means nothing here */
#define RTLD_GLOBAL     0x00100
#define RTLD_LOCAL      0
#define RTLD_NODELETE   0x01000

#define RTLD_DEFAULT    ((void *)0)
#define RTLD_NEXT       ((void *)-1)

typedef struct {
    const char *dli_fname;      /* the object's path                    */
    void       *dli_fbase;      /* where it is loaded                   */
    const char *dli_sname;      /* the nearest symbol at or below, or 0 */
    void       *dli_saddr;      /* its address                          */
} Dl_info;
typedef Dl_info Dl_info_t;

_BEGIN_STD_C

void *dlopen(const char *__file, int __mode);
void *dlsym(void *__restrict __handle, const char *__restrict __name);
int   dlclose(void *__handle);
char *dlerror(void);
int   dladdr(const void *__addr, Dl_info *__info);

_END_STD_C

#endif /* _DLFCN_H_ */
