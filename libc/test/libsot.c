/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * libsot.c - a shared library for sotest, built three times: libsot.so
 * with SOT_VALUE 1, libsot2.so with 2, and libsot3.so lacking sot_bump. Replacing the one with the other on
 * the disk, between two runs of sotest, is how the test sees that the
 * kernel's cache of shared pages notices a file changing.
 *
 * Deliberately touches every kind of thing a library can have: a
 * function, a variable the program reads directly (a COPY relocation),
 * a variable only the library touches, a constructor, and a call into
 * libc.so -- one library depending on another.
 */
#include <stdio.h>
#include <stdlib.h>

int sot_counter = 41;                   /* the program reads it directly */
static int ctor_ran;

__attribute__((constructor)) static void sot_init(void)
{
    ctor_ran = 1;
}

int sot_value(void)
{
    return SOT_VALUE;
}

/* Left out of libsot3.so, so that a program needing it cannot start. */
#ifndef SOT_NO_BUMP
int sot_bump(void)
{
    return ++sot_counter;
}
#endif

int sot_ctor_ran(void)
{
    return ctor_ran;
}

int sot_print(const char *s)
{
    return printf("libsot: %s\n", s);
}

/*
 * Addresses IN the libraries' text, for sotest to ask the kernel about.
 * Not "&printf" in the program: a function whose address a program
 * takes is, by the ELF rules, its PLT entry in the PROGRAM, so that
 * every module agrees on one address for it -- and that page is the
 * program's own. And on this target the linker gives EVERY function a
 * program calls a PLT address in its symbol table, so the only libc
 * function whose real address a library sees is one the program never
 * mentions at all: bsearch, here. sot_here is static, so its address is
 * libsot's own text.
 */
static int sot_here(void)
{
    return 0;
}

const void *sot_libc_text(void)
{
    return (const void *)&bsearch;
}

const void *sot_own_text(void)
{
    return (const void *)&sot_here;
}

/* bsearch, called from here, so the program need not name it. */
static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int sot_bsearch_works(void)
{
    static const int v[] = { 1, 3, 5, 7, 9 };
    int key = 7;
    const int *hit = bsearch(&key, v, 5, sizeof(int), cmp_int);

    return hit == &v[3];
}

/* sot_value's address as this library sees it, through its GOT. C says
 * a function has one address; the program must see the same one. */
const void *sot_value_addr(void)
{
    return (const void *)&sot_value;
}
