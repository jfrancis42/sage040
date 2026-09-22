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
 * stdio_ext.h -- picolibc 1.8.12 has none. The interface glibc, musl and
 * Solaris give for asking a FILE about its state; gnulib uses it in
 * preference to reaching inside FILE, which it cannot do for picolibc.
 * Implemented in libos/linux/machine/m68k/stdio-ext.c.
 */
#ifndef _STDIO_EXT_H_
#define _STDIO_EXT_H_

#include <stdio.h>

_BEGIN_STD_C

#define FSETLOCKING_QUERY    0
#define FSETLOCKING_INTERNAL 1
#define FSETLOCKING_BYCALLER 2

size_t __fbufsize(FILE *);
int    __freading(FILE *);
int    __fwriting(FILE *);
int    __freadable(FILE *);
int    __fwritable(FILE *);
int    __flbf(FILE *);
void   __fpurge(FILE *);
size_t __fpending(FILE *);
size_t __freadahead(FILE *);
int    __fsetlocking(FILE *, int);
void   _flushlbf(void);
/* In parentheses: <stdio.h> has a macro of this name. */
void   (__fseterr)(FILE *);

_END_STD_C

#endif /* _STDIO_EXT_H_ */
