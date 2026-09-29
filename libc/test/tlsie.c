/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tlsie.c - libtlsie.so, built -ftls-model=initial-exec: its TLS is
 * reached at a fixed distance from the thread pointer, which only a
 * library present at start can have. dlopen must refuse it -- and
 * leave nothing of it behind.
 */
static __thread int ie_var = 5;

int *ie_addr(void)
{
    return &ie_var;
}
