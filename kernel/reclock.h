/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * reclock.h - POSIX and OFD record locks. See reclock.c.
 */
#ifndef RECLOCK_H
#define RECLOCK_H

#include "kernel.h"
#include "uapi.h"

struct file;

/* fcntl's lock commands. `wide`: the call was fcntl64, whose OFD
 * commands take a flock64 (its F_*LK64 always do, and its plain F_*LK
 * never). */
int  reclock_fcntl(int fd, int cmd, u32 uarg, int wide);

/* A descriptor for `f` was closed by process `pid`: every POSIX lock
 * that process holds on the file goes. */
void reclock_closed(struct file *f, int pid);

/* The last close of a description: its OFD locks go. */
void reclock_file_gone(struct file *f);

/* Process `pid` is gone. */
void reclock_exit(int pid);

#endif /* RECLOCK_H */
