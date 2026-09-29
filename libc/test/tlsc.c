/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/* tlsc.c - libtlsc.so: what libtlsb.so needs, so that dlopen of b has a
 * dependency of its own to load. */
int c_func(void)
{
    return 77;
}
