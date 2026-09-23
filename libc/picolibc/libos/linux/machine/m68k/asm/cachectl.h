/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2026 Jeff Francis
 *
 * asm/cachectl.h - the arguments to cacheflush(2).
 *
 * This is Linux/m68k's header, at Linux/m68k's path, with Linux's
 * values, because that is what asks for it: libffi writes a trampoline
 * and then calls SYS_cacheflush to make the 68040 see the bytes it just
 * stored as instructions rather than as data. Anything else that
 * generates code at run time needs the same.
 *
 * Why the call has to exist at all, and what this kernel currently does
 * about it, is in kernel/cache.c.
 */
#ifndef _ASM_M68K_CACHECTL_H
#define _ASM_M68K_CACHECTL_H

/* How much to flush. */
#define FLUSH_SCOPE_LINE    1   /* the cache lines the range covers */
#define FLUSH_SCOPE_PAGE    2   /* the pages the range covers       */
#define FLUSH_SCOPE_ALL     3   /* everything                       */

/* Which cache. The 68040 has two, and code written as data is exactly
 * the case where the difference matters. */
#define FLUSH_CACHE_DATA    1
#define FLUSH_CACHE_INSN    2
#define FLUSH_CACHE_BOTH    3

#ifndef __ASSEMBLER__
#include <sys/cdefs.h>
#include <stddef.h>

_BEGIN_STD_C
int cacheflush(void *__addr, int __scope, int __cache, size_t __len);
_END_STD_C
#endif

#endif /* _ASM_M68K_CACHECTL_H */
