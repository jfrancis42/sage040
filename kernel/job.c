/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * job.c - the job table, and where a signal actually lands.
 */
#include "job.h"
#include "exec.h"
#include "tty.h"
#include "syscall.h"
#include "console.h"
#include "errno.h"
#include "string.h"

static struct job jobs[JOB_MAX];
static int next_id = 1;
static int fg_id;

/* ---------------------------------------------------------------- */
/* The table                                                         */
/* ---------------------------------------------------------------- */

struct job *job_get(int id)
{
    int i;

    if (id <= 0) {
        return 0;
    }
    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state != JOB_FREE && jobs[i].id == id) {
            return &jobs[i];
        }
    }
    return 0;
}

struct job *job_nth(int index)
{
    int i;

    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state != JOB_FREE && index-- == 0) {
            return &jobs[i];
        }
    }
    return 0;
}

int job_create(const char *cmd, int background)
{
    int i;

    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state == JOB_FREE) {
            struct job *j = &jobs[i];

            memset(j, 0, sizeof(*j));
            j->id = next_id++;
            j->state = JOB_NEW;
            j->background = background;
            strncpy(j->cmd, cmd, JOB_CMD_MAX - 1);
            j->cmd[JOB_CMD_MAX - 1] = '\0';
            return j->id;
        }
    }
    return -EAGAIN;
}

void job_reap(void)
{
    int i;

    for (i = 0; i < JOB_MAX; i++) {
        if (jobs[i].state == JOB_DONE) {
            jobs[i].state = JOB_FREE;
        }
    }
    /*
     * The numbering starts over once the table is empty, the way a shell
     * does not -- but a shell has a process table to keep them unique
     * against and this does not, and a job number that climbs forever on
     * a machine that can hold four of them reads badly.
     */
    if (!job_nth(0)) {
        next_id = 1;
    }
}

int job_foreground(void)
{
    return fg_id;
}

void job_set_foreground(int id)
{
    fg_id = id;
}

/* ---------------------------------------------------------------- */
/* Signals                                                           */
/* ---------------------------------------------------------------- */

void job_signal_fg(int sig)
{
    struct job *j = job_get(fg_id);

    /*
     * No foreground job means the shell itself is reading, and the shell
     * is not a job. Recording it would be wrong -- it would fire at the
     * next program the person ran, which is a genuinely confusing bug.
     * The reader finds out from the EINTR that tty.c returns.
     */
    if (!j || j->state != JOB_RUNNING) {
        return;
    }
    /*
     * Last one wins rather than queueing. The only two that get here are
     * interrupt and stop, and somebody who pressed both wants the
     * second: pressing ctrl-C after ctrl-Z means "no, just kill it".
     */
    j->pending = sig;
}

void job_kill_fg(int sig)
{
    struct job *j = job_get(fg_id);

    if (!j) {
        return;
    }
    j->pending = 0;
    j->state = JOB_DONE;
    j->signalled = sig;
    j->status = 128 + sig;
}

int job_signal_pending(void)
{
    struct job *j = job_get(fg_id);

    return j && j->pending;
}

const char *job_state_name(int state)
{
    switch (state) {
    case JOB_NEW:     return "queued";
    case JOB_RUNNING: return "running";
    case JOB_STOPPED: return "stopped";
    case JOB_DONE:    return "done";
    default:          return "?";
    }
}

const char *job_signal_name(int sig)
{
    switch (sig) {
    case SIGINT:  return "interrupt";
    case SIGKILL: return "killed";
    case SIGTERM: return "terminated";
    case SIGTSTP: return "stopped";
    case SIGSEGV: return "segmentation fault";
    case SIGILL:  return "illegal instruction";
    case SIGFPE:  return "arithmetic exception";
    case SIGCONT: return "continued";
    default:      return "signal";
    }
}

/*
 * Do what the signal says.
 *
 * Every path out of here that touches the program is a one-way jump into
 * exec.c, so the ordering matters: the job's own state is settled first,
 * because after the jump this function does not exist any more.
 */
void job_deliver(int site)
{
    struct job *j = job_get(fg_id);
    int sig;

    if (!j || j->state != JOB_RUNNING || !j->pending) {
        return;
    }
    sig = j->pending;

    if (sig == SIGTSTP) {
        /*
         * Stopping means coming back later, and coming back means
         * having somewhere to come back to. At a system call boundary
         * there is one; from a timer interrupt there is not, short of
         * saving every register and the program counter, which is a
         * context switch and belongs to the scheduler.
         *
         * So a program that makes no system calls cannot be stopped.
         * The signal stays pending until it makes one -- and if it never
         * does, ctrl-C still works on it, because killing something does
         * not require it to be resumable.
         */
        if (site != JOB_AT_SYSCALL) {
            return;             /* stays pending */
        }
        j->pending = 0;
        j->state = JOB_STOPPED;
        j->signalled = SIGTSTP;
        kputs("\n[");
        kputdec((u32)j->id);
        kputs("]+  stopped   ");
        kputln(j->cmd);
        /*
         * It is stopping inside a system call, and has to come back to
         * exactly that depth -- the shell is about to run with a count
         * of its own.
         */
        j->depth = syscall_depth();
        exec_stop(&j->saved_sp);
        /*
         * Reached again when fg resumes it. The job is running once
         * more and the system call it was in returns normally.
         */
        j->state = JOB_RUNNING;
        j->signalled = 0;
        return;
    }

    /*
     * Killing. Safe from either site: the program's whole stack is
     * discarded, and both callers have established that the kernel is
     * not in the middle of anything that would be left half done.
     */
    (void)site;
    j->pending = 0;
    j->state = JOB_DONE;
    j->signalled = sig;
    j->status = 128 + sig;
    kputc('\n');
    exec_kill(j->status);
}
