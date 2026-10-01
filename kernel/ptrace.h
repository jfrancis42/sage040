/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ptrace.h - one process watching and steering another (ptrace(2)).
 *
 * Linux's interface, request for request and number for number --
 * m68k's, which is the generic set plus GETREGS..SETFPREGS at 12..15 --
 * so that strace and gdb built for Linux/m68k drive it unchanged. See
 * ptrace.c for what each stop is and where it happens.
 */
#ifndef PTRACE_H
#define PTRACE_H

#include "kernel.h"

struct task;
struct pt_regs;

#define PTRACE_TRACEME          0
#define PTRACE_PEEKTEXT         1
#define PTRACE_PEEKDATA         2
#define PTRACE_PEEKUSR          3
#define PTRACE_POKETEXT         4
#define PTRACE_POKEDATA         5
#define PTRACE_POKEUSR          6
#define PTRACE_CONT             7
#define PTRACE_KILL             8
#define PTRACE_SINGLESTEP       9
#define PTRACE_GETREGS          12
#define PTRACE_SETREGS          13
#define PTRACE_GETFPREGS        14
#define PTRACE_SETFPREGS        15
#define PTRACE_ATTACH           16
#define PTRACE_DETACH           17
#define PTRACE_SYSCALL          24
#define PTRACE_SETOPTIONS       0x4200
#define PTRACE_GETEVENTMSG      0x4201
#define PTRACE_GETSIGINFO       0x4202
#define PTRACE_SETSIGINFO       0x4203

#define PTRACE_O_TRACESYSGOOD   0x01
#define PTRACE_O_TRACEFORK      0x02
#define PTRACE_O_TRACEVFORK     0x04
#define PTRACE_O_TRACECLONE     0x08
#define PTRACE_O_TRACEEXEC      0x10
#define PTRACE_O_TRACEVFORKDONE 0x20
#define PTRACE_O_TRACEEXIT      0x40
#define PTRACE_O_EXITKILL       0x100000
#define PTRACE_O_MASK           (0x7f | PTRACE_O_EXITKILL)

#define PTRACE_EVENT_FORK       1
#define PTRACE_EVENT_VFORK      2
#define PTRACE_EVENT_CLONE      3
#define PTRACE_EVENT_EXEC       4
#define PTRACE_EVENT_EXIT       6

/* Linux/m68k's PEEKUSER offsets, in longs: struct user_regs_struct. */
#define PT_D1       0
#define PT_A0       7
#define PT_D0       14
#define PT_USP      15
#define PT_ORIG_D0  16
#define PT_SR       17
#define PT_PC       18
#define PT_NREGS    19          /* what GETREGS copies               */
#define PT_FP0      21          /* fp0-fp7, three longs each          */
#define PT_FPCR     45
#define PT_FPSR     46
#define PT_FPIAR    47

/* t->ptrace, what the tracer has asked for. */
#define PT_SYSCALL  0x1         /* stop at the next syscall entry/exit */
#define PT_STEP     0x2         /* one instruction, then SIGTRAP       */

/* ptrace(2) itself, with Linux's argument order. */
s32  sys_ptrace(int req, int pid, u32 addr, u32 data);

/*
 * The stops, called from where each happens. Each does nothing for a
 * task nobody traces, so the callers need not ask first -- though the
 * syscall ones are on every system call's path, and inline the test.
 */
int  ptrace_syscall_enter(struct pt_regs *regs);
#define PTRACE_SYSCALL_RUN      0   /* no stop: run the call as it is   */
#define PTRACE_SYSCALL_STOPPED  1   /* stopped: read the call again     */
#define PTRACE_SYSCALL_SKIP     2   /* the tracer said ORIG_D0 = -1     */
void ptrace_syscall_exit(struct pt_regs *regs);
int  ptrace_signal(int sig, struct pt_regs *regs); /* the signal to act on   */
void ptrace_exec(struct pt_regs *regs);
void ptrace_fork(struct task *child, u32 clone_flags, int vfork,
                 struct pt_regs *regs);
void ptrace_exiting(struct task *t);               /* tracer or tracee       */

/* For waitpid: a trace stop not yet reported to `tracer`, as a status
 * word; 0 if none. */
int  ptrace_wait_status(struct task *t, struct task *tracer, int *status);

#endif /* PTRACE_H */
