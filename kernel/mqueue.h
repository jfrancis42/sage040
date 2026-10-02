/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
#ifndef MQUEUE_H
#define MQUEUE_H

#include "kernel.h"

struct task;

/* POSIX message queues (mqueue.c): Linux's six calls, the name without
 * its leading slash. time64 says which struct timespec a timeout is. */
s32  sys_mq_open(u32 uname, int oflag, u32 mode, u32 uattr);
s32  sys_mq_unlink(u32 uname);
s32  sys_mq_timedsend(int fd, u32 ubuf, u32 len, u32 prio, u32 uts, int time64);
s32  sys_mq_timedreceive(int fd, u32 ubuf, u32 len, u32 uprio, u32 uts,
                         int time64);
s32  sys_mq_notify(int fd, u32 uev);
s32  sys_mq_getsetattr(int fd, u32 unew, u32 uold);
void mq_task_exit(struct task *t);

#endif /* MQUEUE_H */
