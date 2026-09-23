/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * cache.h - the 68040's caches. See cache.c for what is and is not
 * true about them on this machine today.
 */
#ifndef SAGE040_CACHE_H
#define SAGE040_CACHE_H

#include "../types.h"

/* Push and invalidate both caches, whole. Supervisor only. */
void cache_flush_all(void);

/* cacheflush(2), Linux/m68k's. */
int do_cacheflush(u32 addr, int scope, int cache, u32 len);

#endif
