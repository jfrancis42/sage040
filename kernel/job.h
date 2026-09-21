/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * job.h - what a terminal can do something to.
 *
 * A job is one command the shell started. There is a table of them
 * because a shell needs to name them -- `fg %2` has to mean something --
 * and because ctrl-C and ctrl-Z have to be aimed somewhere. The
 * terminal knows which job is in the foreground and sends signals
 * there; it does not know what a job is made of.
 *
 * This is NOT a process table. A job has no address space of its own, no
 * descriptors of its own and no priority, because there is no MMU turned
 * on and no scheduler. What it has is the part that the terminal needs
 * to exist before ctrl-C can mean anything at all, and the part a
 * scheduler will need to attach to later: an identity, a state, a place
 * to record a signal, and a saved context to come back to.
 *
 * WHAT WORKS TODAY, AND WHAT DOES NOT. One job runs at a time. ctrl-C
 * kills it, from anywhere -- the timer interrupt forces the unwind if it
 * is not making system calls. ctrl-Z stops it and `fg` resumes it, but
 * only at a system call boundary, because that is the only place the
 * kernel is at a known point in its own C code. A stopped job keeps the
 * program area, so nothing else can be started until it finishes; that
 * restriction goes away with an MMU and it is enforced rather than
 * documented. `bg` and `&` are parsed, tracked and refused with a
 * reason: nothing can run in the background until something can
 * schedule it.
 *
 * The signal numbers are Linux's. There is no sigaction() and no
 * handler -- a signal here is something done TO a job, not something a
 * program catches -- which is enough for a terminal and is the half
 * that has to come first.
 */
#ifndef JOB_H
#define JOB_H

#include "kernel.h"
#include "uapi.h"

struct addrspace;

#define JOB_MAX      4
#define JOB_CMD_MAX  128

enum job_state {
    JOB_FREE = 0,
    JOB_NEW,                    /* created, never started             */
    JOB_RUNNING,
    JOB_STOPPED,                /* ctrl-Z, resumable with fg          */
    JOB_DONE
};

struct job {
    int  id;                    /* 1-based, the way a shell numbers   */
    int  state;
    int  background;            /* started with &                     */
    int  status;                /* exit status once JOB_DONE          */
    int  signalled;             /* the signal that ended it, or 0     */
    volatile int pending;       /* raised, not yet delivered          */
    u32  saved_sp;              /* its context while JOB_STOPPED      */
    /*
     * Its address space. A stopped job still owns one, with every page
     * it had -- which is what makes `fg` able to resume into a program
     * that still has its memory, and what a scheduler will swap between.
     */
    struct addrspace *as;

    /*
     * Its supervisor stack: where its traps and interrupts land. A job
     * has one of its own so that a stopped program's saved frames are
     * not overwritten by whoever carries on running.
     */
    u32  kstack;                /* base of the block, guard page first */
    int  depth;                 /* how deep in the kernel it stopped   */
    char cmd[JOB_CMD_MAX];      /* the command line, for fg and jobs  */
};

/* Allocate a slot. Returns the id, or -errno. */
int  job_create(const char *cmd, int background);
struct job *job_get(int id);

/* Walk them, for `jobs`. Index from 0; returns 0 when there are no
 * more. Includes finished ones until they are reaped. */
struct job *job_nth(int index);

/* Forget every job that has finished, which is what a shell does after
 * it has reported them. */
void job_reap(void);

/* Which one the terminal's signals go to. 0 means the shell itself,
 * which is not a job and cannot be signalled. */
int  job_foreground(void);
void job_set_foreground(int id);

/*
 * Raise a signal on the foreground job.
 *
 * CALLED FROM INTERRUPT CONTEXT as well as from a read, so it does
 * nothing but record the number. Everything that actually happens to a
 * program happens in job_deliver(), at a point where the kernel knows
 * where it is.
 */
void job_signal_fg(int sig);

/*
 * Record that the foreground job died of this, with no chance to be
 * delivered anything. Used by the fault handler, which is not raising a
 * signal for the program to receive -- the program is already over -- but
 * saying what ended it, so `jobs` and the shell can report it.
 */
void job_kill_fg(int sig);

/* Is one waiting? Cheap enough to ask on every system call. */
int  job_signal_pending(void);

/*
 * Where delivery is being attempted from.
 *
 * Not a detail: the two things a signal can do have different
 * requirements, and the site is what says which are met.
 *
 *   JOB_AT_SYSCALL   the boundary of a system call. The kernel has
 *                    finished whatever it was doing and is at a C call
 *                    boundary with its own stack intact, so the program
 *                    can be killed AND stopped from here.
 *
 *   JOB_AT_TICK      the timer interrupt, with the program running its
 *                    own code. Everything can be discarded, so a kill is
 *                    safe -- and this is the only reason a program that
 *                    makes no system calls is interruptible at all. A
 *                    stop is not: there is no way back into the middle
 *                    of an interrupted instruction stream without saving
 *                    a full register set, which is the scheduler's job
 *                    and not this one's.
 *
 * The caller must not use JOB_AT_TICK while the kernel is inside a
 * system call -- the filesystem may be halfway through a directory
 * entry, and the unwind would leave it there.
 */
#define JOB_AT_SYSCALL  0
#define JOB_AT_TICK     1

/* Act on a waiting signal. Does not return if it kills the job; returns
 * later, from fg, if it stops one. */
void job_deliver(int site);

const char *job_state_name(int state);
const char *job_signal_name(int sig);

#endif /* JOB_H */
