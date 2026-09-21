/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * block.h - the kernel's block device interface.
 *
 * One device, the ATA disk.  The filesystem talks to this and to nothing
 * else, so a second kind of disk means another implementation of these
 * five calls and no change above.
 */
#ifndef BLOCK_H
#define BLOCK_H

#include "kernel.h"

#define BLK_SECTOR_SIZE  512

int  blk_init(void);                             /* 0 on success        */
int  blk_read(u32 lba, u32 count, void *buf);
int  blk_write(u32 lba, u32 count, const void *buf);
u32  blk_capacity(void);                         /* sectors             */
const char *blk_model(void);                     /* IDENTIFY model      */

#endif /* BLOCK_H */
