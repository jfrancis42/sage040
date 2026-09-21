/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * signal.h - telling a task something happened.
 *
 * What existed before was one-directional: the terminal set a flag and
 * the kernel did something to the foreground job. This is the general
 * form -- any task can signal any task, every signal has a default
 * action, and a task can block or ignore what it chooses.
 *
 * WHAT IS DELIBERATELY NOT HERE: handlers. Catching a signal means
 * building a frame on the USER stack, returning to a trampoline in user
 * memory, running the handler, and coming back through a second system
 * call -- and every part of that is a place to get the stack wrong. The
 * default actions cover what this machine actually needs: ctrl-C ends a
 * program, ctrl-Z stops it, fg continues it, a fault kills it, and a
 * parent learns that a child finished. sigaction() is the next thing
 * here rather than an omission.
 *
 * DELIVERY IS AT THE BOUNDARY, never where the signal is raised.
 * kill() from one task, or the terminal from an interrupt, does nothing
 * but set a bit -- because the target may be halfway through a system
 * call, and ending it there would leave whatever it was doing half
 * done. The bit is acted on when that task is next about to return to
 * user mode, which is the same place preemption happens and for the
 * same reason.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

#include "kernel.h"
#include "uapi.h"

struct task;

#define SIGMASK(n)  (1UL << (n))

/* Raise it. Returns 0, or -ESRCH if there is no such task. */
int  signal_send(struct task *t, int sig);
int  signal_kill(int pid, int sig);

/*
 * Act on anything pending for the current task.
 *
 * Called from task_ret_to_user(). May not return: a signal whose default
 * action is to terminate does exactly that.
 */
void signal_deliver(void);

/* Is there something waiting that would interrupt a sleep? A blocked
 * task is woken for this and its system call returns -EINTR. */
int  signal_pending(struct task *t);

const char *signal_name(int sig);

#endif /* SIGNAL_H */
