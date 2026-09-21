/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * malloc.h - the allocator. See malloc.c; it is meant to be replaced by
 * the C library's own when there is one.
 */
#ifndef MALLOC_H
#define MALLOC_H

#include "types.h"

void  *malloc(u32 n);
void   free(void *p);
void  *calloc(u32 count, u32 size);
void  *realloc(void *p, u32 n);

/*
 * Walk every segment and every free list and verify the invariants.
 * 0 if all is well; otherwise -1, with what is wrong on stderr.
 */
int    malloc_check(void);

/* glibc's names and meanings, in bytes. */
struct mallinfo {
    int arena;                  /* heap obtained with sbrk             */
    int ordblks;                /* free blocks                         */
    int smblks;                 /* always 0: no fastbins here          */
    int hblks;                  /* blocks obtained with mmap           */
    int hblkhd;                 /* bytes obtained with mmap            */
    int usmblks;                /* always 0                            */
    int fsmblks;                /* always 0                            */
    int uordblks;               /* heap bytes in use, headers included */
    int fordblks;               /* heap bytes free                     */
    int keepcost;               /* always 0                            */
};

struct mallinfo mallinfo(void);

#endif /* MALLOC_H */
