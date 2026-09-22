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

#endif /* PIPE_H */
