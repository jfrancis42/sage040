/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * signal.c - raising, and acting on.
 */
#include "signal.h"
#include "task.h"
#include "wait.h"
#include "console.h"
#include "errno.h"
#include "string.h"

/*
 * What a signal does when nothing has asked for anything else.
 *
 * The table is the whole of the policy, and it is worth being able to
 * read it in one place rather than inferring it from a switch buried in
 * the delivery path.
 */
enum { SIG_TERM, SIG_STOP, SIG_CONT, SIG_IGN };

static int default_action(int sig)
{
    switch (sig) {
    case SIGTSTP: return SIG_STOP;
    case SIGCONT: return SIG_CONT;
    case SIGCHLD: return SIG_IGN;
    default:      return SIG_TERM;
    }
}

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGINT:  return "interrupt";
    case SIGILL:  return "illegal instruction";
    case SIGFPE:  return "arithmetic exception";
    case SIGKILL: return "killed";
    case SIGSEGV: return "segmentation fault";
    case SIGPIPE: return "broken pipe";
    case SIGTERM: return "terminated";
    case SIGCHLD: return "child exited";
    case SIGCONT: return "continued";
    case SIGTSTP: return "stopped";
    default:      return "signal";
    }
}

int signal_send(struct task *t, int sig)
{
    u16 sr;

    if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) {
        return -ESRCH;
    }
    if (sig <= 0 || sig >= 32) {
        return -EINVAL;
    }

    sr = irq_save();

    /*
     * SIGKILL cannot be blocked or ignored, which is the one guarantee
     * that makes it worth having: everything else a task can decline,
     * so there has to be something it cannot.
     */
    if (sig != SIGKILL && (t->sig_ignored & SIGMASK(sig))) {
        irq_restore(sr);
        return 0;
    }

    t->sig_pending |= SIGMASK(sig);

    /*
     * A continue cancels a pending stop and vice versa. Delivering both
     * would leave the task in whichever state happened to be acted on
     * second, which is not what either sender asked for.
     */
    if (sig == SIGCONT) {
        t->sig_pending &= ~SIGMASK(SIGTSTP);
        if (t->state == TASK_STOPPED) {
            t->state = TASK_READY;
        }
    } else if (sig == SIGTSTP) {
        t->sig_pending &= ~SIGMASK(SIGCONT);
    }

    /*
     * A task asleep has to be woken to find out. Its system call
     * returns -EINTR, the way a real one does, and the signal is acted
     * on at the boundary it returns through.
     */
    if (t->state == TASK_BLOCKED && sig != SIGCHLD) {
        if (t->queue) {
            wake_all(t->queue);
        } else {
            t->state = TASK_READY;
        }
    }

    irq_restore(sr);
    return 0;
}

int signal_kill(int pid, int sig)
{
    return signal_send(task_find(pid), sig);
}

int signal_pending(struct task *t)
{
    if (!t) {
        return 0;
    }
    /* SIGKILL is never blocked, so it counts even against the mask. */
    return (t->sig_pending & ~t->sig_blocked) != 0 ||
           (t->sig_pending & SIGMASK(SIGKILL)) != 0;
}

void signal_deliver(void)
{
    struct task *t = current;
    int sig;

    if (!t) {
        return;
    }

    for (sig = 1; sig < 32; sig++) {
        u32 bit = SIGMASK(sig);

        if (!(t->sig_pending & bit)) {
            continue;
        }
        if (sig != SIGKILL && (t->sig_blocked & bit)) {
            continue;
        }
        t->sig_pending &= ~bit;

        switch (default_action(sig)) {
        case SIG_IGN:
            break;

        case SIG_CONT:
            if (t->state == TASK_STOPPED) {
                t->state = TASK_READY;
            }
            t->stop_reported = 0;
            break;

        case SIG_STOP:
            /*
             * Stopping is a state, not an unwind: the task stays
             * exactly where it is, in the middle of whatever system
             * call it was making, and a later SIGCONT resumes it there.
             * That is only possible because it has a kernel stack of its
             * own to be left sitting on.
             */
            t->signalled = sig;
            t->state = TASK_STOPPED;
            t->stop_reported = 0;
            if (t->parent) {
                signal_send(t->parent, SIGCHLD);
                wake_all(&t->parent->child_wait);
            }
            schedule();
            t->signalled = 0;
            break;

        case SIG_TERM:
        default:
            t->signalled = sig;
            task_exit(128 + sig);       /* does not return */
        }
    }
}
