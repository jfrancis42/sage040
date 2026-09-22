/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * signal.h - telling a task something happened.
 *
 * Any task can signal any user task, every signal has a default action,
 * and a program can block, ignore or catch what it chooses -- with
 * Linux's numbers, masks, sigaction layout and sigreturn frame, so a
 * ported program's signal code works unchanged.
 *
 * DELIVERY IS AT THE BOUNDARY, never where the signal is raised.
 * kill() from one task, or the terminal from an interrupt, does nothing
 * but set a bit -- because the target may be halfway through a system
 * call, and ending it there would leave whatever it was doing half
 * done. The bit is acted on when that task is next about to return to
 * user mode, which is the same place preemption happens and for the
 * same reason. See signal.c for how a handler is run and how an
 * interrupted call is restarted.
 */
#ifndef SIGNAL_H
#define SIGNAL_H

#include "kernel.h"
#include "uapi.h"

struct task;

/* Signal N is bit N-1, as in Linux's old_sigset_t. */
#define SIGMASK(n)  (1UL << ((n) - 1))

/* What can never be blocked, caught or ignored. */
#define SIG_UNBLOCKABLE (SIGMASK(SIGKILL) | SIGMASK(SIGSTOP))

/* Raise it. Returns 0, or -ESRCH if there is no such task. */
int  signal_send(struct task *t, int sig);
int  signal_kill(int pid, int sig);

/* To every user task in process group `pgid`. -ESRCH if there are none;
 * signal 0 only asks whether there are. */
int  signal_group(int pgid, int sig);

/*
 * Act on anything pending for the current task, which is about to
 * return to user mode with the registers in `regs`.
 *
 * Called from task_ret_to_user(). May not return: a signal whose default
 * action is to terminate does exactly that. Running a handler rewrites
 * `regs` so the return lands in it.
 */
struct pt_regs;
void signal_deliver(struct pt_regs *regs);

/* The system calls. Each returns what the call returns. */
int  signal_set_action(int sig, const struct sigaction *act,
                       struct sigaction *old);
int  signal_procmask(int how, const u32 *set, u32 *old);
u32  signal_pending_set(void);
int  signal_pause(void);
int  signal_suspend(u32 mask);
s32  signal_return(struct pt_regs *regs);
s32  signal_rt_return(struct pt_regs *regs);

/*
 * Returned by pause() and sigsuspend() inside the kernel, never seen
 * by a program: it becomes -EINTR if a handler ran and a transparent
 * restart if none did. Linux's name and number.
 */
#define ERESTARTNOHAND  514

/* Is there something waiting that would interrupt a sleep? A blocked
 * task is woken for this and its system call returns -EINTR. */
int  signal_pending(struct task *t);

const char *signal_name(int sig);

#endif /* SIGNAL_H */
