/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * string.h - the handful of C library functions the kernel needs.
 *
 * There is no C library here, and these keep the standard names on
 * purpose: GCC emits calls to memcpy and memset of its own accord, for
 * structure assignment and array initialisation, and those calls have to
 * resolve to something.
 */
#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include "kernel.h"

typedef unsigned long ksize_t;

void *memcpy(void *dst, const void *src, ksize_t n);
void *memmove(void *dst, const void *src, ksize_t n);
void *memset(void *dst, int c, ksize_t n);
int   memcmp(const void *a, const void *b, ksize_t n);

ksize_t strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, ksize_t n);
char   *strcpy(char *dst, const char *src);
int     stricmp(const char *a, const char *b);

#endif /* KERNEL_STRING_H */
