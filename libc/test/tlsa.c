/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tlsa.c - libtlsa.so, a library with thread-local storage that the
 * program is LINKED against: its TLS is in every thread's static block,
 * and the program reaches a_var directly (initial-exec, a TPREL32
 * relocation ld.so fills in) while the library's own code reaches it
 * through __tls_get_addr. tlstest checks that the two agree.
 */
__thread int a_var = 100;
__thread int a_bss;

int *a_addr(void)
{
    return &a_var;
}

int a_get(void)
{
    return a_var + a_bss;
}
