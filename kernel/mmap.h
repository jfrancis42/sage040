/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mmap.h - mapping memory into the current task's address space.
 *
 * The three calls with Linux's meaning. Each returns what the system
 * call returns: an address or 0, or a negated errno. `offset` is in
 * bytes; mmap2's page offset is converted by the caller.
 */
#ifndef MMAP_H
#define MMAP_H

#include "kernel.h"

s32 do_mmap(u32 addr, u32 len, u32 prot, u32 flags, int fd, u32 offset);
int do_munmap(u32 addr, u32 len);
int do_mprotect(u32 addr, u32 len, u32 prot);
int do_msync(u32 addr, u32 len, u32 flags);
s32 do_mremap(u32 old, u32 old_len, u32 new_len, u32 flags, u32 new_addr);

#endif /* MMAP_H */
