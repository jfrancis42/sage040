/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pipe.h - pipes. See pipe.c.
 */
#ifndef PIPE_H
#define PIPE_H

#include "kernel.h"

/*
 * A new pipe, as two descriptors in the current task: fds[0] reads and
 * fds[1] writes. `flags` may carry O_NONBLOCK and O_CLOEXEC, for both.
 */
int pipe_create(int fds[2], int flags);

/*
 * socketpair(AF_UNIX, SOCK_STREAM, ...): two connected ends. `type` may
 * carry SOCK_NONBLOCK and SOCK_CLOEXEC.
 */
int usock_pair(int type, int fds[2]);

/* The socket calls, for an end of a pair. */
struct file;
int usock_is(struct file *f);
s32 usock_send(struct file *f, const void *buf, u32 len, int flags);
s32 usock_recv(struct file *f, void *buf, u32 len, int flags);
int usock_shutdown(struct file *f, int how);

#endif /* PIPE_H */
