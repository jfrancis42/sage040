/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * poll.h - waiting on several descriptors at once. See poll.c.
 */
#ifndef POLL_H
#define POLL_H

#include "kernel.h"
#include "uapi.h"

/* The most descriptors one poll() may name. More than OPEN_MAX, because
 * a program may name the same one twice, or ones it has closed. */
#define POLL_MAX    64

/*
 * Fill in revents for `n` descriptors, waiting up to `timeout_ms` (-1:
 * for ever, 0: not at all) for at least one to be ready. Returns how
 * many are, 0 on timeout, or -ERESTARTNOHAND for a signal. `left_ms`,
 * if given, receives the time that was not used.
 */
int  poll_files(struct pollfd *fds, u32 n, s32 timeout_ms, s32 *left_ms);

/* select() over kernel copies of the three sets, rewritten in place. */
int  poll_select(u32 nfds, u32 *in, u32 *out, u32 *ex, s32 timeout_ms,
                 s32 *left_ms);

/* POLL* bits for one descriptor of the current task, now. */
int  poll_fd(int fd);

/* Something may have become ready: anyone polling should look again. */
void poll_wake(void);

#endif /* POLL_H */
