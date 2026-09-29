/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tlsb.c - libtlsb.so, loaded only by dlopen: its TLS has no place in
 * the static block, so each thread's copy is made the first time that
 * thread reaches for it. It needs libtlsc.so, and has a constructor.
 */
extern int c_func(void);

__thread int b_var = 200;
__thread char b_big[3000];
int b_ctor_ran;

__attribute__((constructor)) static void b_init(void)
{
    b_ctor_ran = 1;
}

int *b_addr(void)
{
    return &b_var;
}

char *b_big_addr(void)
{
    return b_big;
}

int b_calls_c(void)
{
    return c_func();
}
