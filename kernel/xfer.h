/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * xfer.h - sendfile, splice, copy_file_range. See xfer.c.
 */
#ifndef XFER_H
#define XFER_H

#include "kernel.h"

/* `wide`: sendfile64, whose offset is a loff_t. */
s32 sys_sendfile(int out, int in, u32 uoff, u32 count, int wide);
s32 sys_splice(int in, u32 uoff_in, int out, u32 uoff_out, u32 len,
               u32 flags);
s32 sys_copy_file_range(int in, u32 uoff_in, int out, u32 uoff_out,
                        u32 len, u32 flags);

#endif /* XFER_H */
