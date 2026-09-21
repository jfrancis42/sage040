/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * version.c - who this kernel is, in one place.
 *
 * The build stamp lives here rather than in a header because __DATE__ and
 * __TIME__ are expanded separately in every translation unit that uses
 * them.  With the stamp in a macro, the banner and the shell's 'uname -a'
 * reported build times a second apart -- true, useless, and
 * exactly the kind of detail that makes someone doubt the rest of the
 * output.
 */
#include "kernel.h"

const char kernel_version[] = KERNEL_VERSION;
const char kernel_build[]   = __DATE__ " " __TIME__;
