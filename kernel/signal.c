/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * signal.c - raising, and acting on.
 *
 * A signal is raised by setting a bit, anywhere, at any time -- from
 * another task, from the terminal's interrupt, from the kernel itself.
 * It is acted on only on the way back to user mode, in signal_deliver(),
 * with the interrupted user context in hand as a struct pt_regs.
 *
 * Acting on one is one of three things: the default action (end, stop,
 * continue, or nothing), nothing at all because it is ignored, or
 * running the program's handler. A handler is run by saving the whole
 * user context on the user stack, pointing the pc at the handler and the
 * stack at that frame, and letting the ordinary return to user mode
 * happen. The handler returns to the library's trampoline, which makes
 * the sigreturn call, which puts the saved context back.
 */
#include "signal.h"
#include "task.h"
#include "wait.h"
#include "ptregs.h"
#include "uaccess.h"
#include "console.h"
#include "errno.h"
#include "string.h"

extern void fpu_save(u32 *area);
extern void fpu_restore(const u32 *area);

#define FPU_IDLE_FRAME  0x41000000UL

/* The 68040's condition codes: X N Z V C. All a program may set in sr. */
#define SR_CCR          0x001f
#define SR_TRACE        0xc000

/*
 * What a signal does when nothing has asked for anything else.
 *
 * The table is the whole of the policy, and it is worth being able to
 * read it in one place rather than inferring it from a switch buried in
 * the delivery path.
 */
enum { SIG_TERM, SIG_STOP, SIG_CONT, SIG_IGNORE };

static int default_action(int sig)
{
    switch (sig) {
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
        return SIG_STOP;
    case SIGCONT:
        return SIG_CONT;
    case SIGCHLD:
    case SIGURG:
    case SIGWINCH:
        return SIG_IGNORE;
    default:
        return SIG_TERM;
    }
}

#define STOP_SIGNALS  (SIGMASK(SIGSTOP) | SIGMASK(SIGTSTP) | \
                       SIGMASK(SIGTTIN) | SIGMASK(SIGTTOU))

const char *signal_name(int sig)
{
    return strsignal(sig);
}

/* Would this signal be thrown away if it were delivered now? */
static int is_ignored(const struct task *t, int sig)
{
    sighandler_t h = t->sigact[sig].sa_handler;

    if (sig == SIGKILL || sig == SIGSTOP) {
        return 0;
    }
    return h == SIG_IGN || (h == SIG_DFL && default_action(sig) == SIG_IGNORE);
}

int signal_send(struct task *t, int sig)
{
    u32 bit;
    u16 sr;

    if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) {
        return -ESRCH;
    }
    if (sig <= 0 || sig >= NSIG) {
        return -EINVAL;
    }

    /*
     * A kernel task never returns to user mode, so a signal sent to one
     * would never be acted on -- and would sit pending for ever, making
     * every sleep it tried return at once. The kernel's own tasks take
     * no signals at all, which is what Linux does with its threads.
     * (It used to work by accident: the system call gate took every
     * kernel task's call for a user one, and delivered on the way out.)
     */
    if (!t->as) {
        return 0;
    }

    bit = SIGMASK(sig);
    sr = irq_save();

    /*
     * A continue cancels a pending stop and vice versa, and a continue
     * resumes a stopped task at the moment it is SENT -- whether it is
     * then caught, ignored or blocked. That is POSIX's rule, and the
     * only way `fg` can work on a program that ignores SIGCONT.
     *
     * SIGKILL resumes a stopped task too, or it would sit pending in a
     * task that is never going to run to act on it.
     */
    if (sig == SIGCONT || sig == SIGKILL) {
        t->sig_pending &= ~STOP_SIGNALS;
        if (t->state == TASK_STOPPED) {
            t->state = TASK_READY;
        }
    } else if (bit & STOP_SIGNALS) {
        t->sig_pending &= ~SIGMASK(SIGCONT);
    }

    /*
     * An ignored signal is discarded when it is SENT, not left pending --
     * both one the task asked to ignore and one whose default is to be
     * ignored, like SIGCHLD. Otherwise a parent that never waits would
     * collect a pending SIGCHLD that interrupts every sleep it makes.
     * That is POSIX's rule and Linux's. A blocked signal is kept, because
     * its disposition may change before it is unblocked.
     */
    if (!(t->sig_blocked & bit) && is_ignored(t, sig)) {
        irq_restore(sr);
        return 0;
    }

    t->sig_pending |= bit;

    /*
     * A task asleep has to be woken to find out. Its system call
     * returns -EINTR, the way a real one does, and the signal is acted
     * on at the boundary it returns through. A blocked signal wakes
     * nothing: the task could not act on it if it did.
     */
    if (t->state == TASK_BLOCKED &&
        (!(t->sig_blocked & bit) || sig == SIGKILL)) {
        wake_signalled(t);
    }

    irq_restore(sr);
    return 0;
}

int signal_kill(int pid, int sig)
{
    struct task *t = task_find(pid);

    if (t && !t->as && t->state != TASK_ZOMBIE) {
        return -EPERM;          /* see signal_send: it would do nothing */
    }
    if (sig == 0) {
        /* kill(pid, 0) asks whether the task exists, and sends nothing. */
        return (t && t->state != TASK_UNUSED && t->state != TASK_ZOMBIE)
               ? 0 : -ESRCH;
    }
    return signal_send(t, sig);
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

/* The next signal to act on: SIGKILL first, then the lowest number. */
static int next_signal(struct task *t)
{
    u32 ready = t->sig_pending & (~t->sig_blocked | SIG_UNBLOCKABLE);
    int sig;

    if (ready & SIGMASK(SIGKILL)) {
        return SIGKILL;
    }
    for (sig = 1; sig < NSIG; sig++) {
        if (ready & SIGMASK(sig)) {
            return sig;
        }
    }
    return 0;
}

/* --- dispositions ---------------------------------------------------- */

int signal_set_action(int sig, const struct sigaction *act,
                      struct sigaction *old)
{
    struct task *t = current;

    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }
    if (old) {
        *old = t->sigact[sig];
    }
    if (!act) {
        return 0;
    }
    if (sig == SIGKILL || sig == SIGSTOP) {
        return -EINVAL;
    }
    if (act->sa_flags & (SA_SIGINFO | SA_ONSTACK)) {
        return -EINVAL;         /* not supported, and said so */
    }
    if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN &&
        (!(act->sa_flags & SA_RESTORER) || !act->sa_restorer)) {
        return -EINVAL;         /* nowhere for the handler to return to */
    }

    t->sigact[sig] = *act;
    t->sigact[sig].sa_mask &= ~SIG_UNBLOCKABLE;

    /* Setting a signal to be ignored discards one already pending. */
    if (is_ignored(t, sig)) {
        t->sig_pending &= ~SIGMASK(sig);
    }
    return 0;
}

int signal_procmask(int how, const u32 *set, u32 *old)
{
    struct task *t = current;

    if (old) {
        *old = t->sig_blocked;
    }
    if (!set) {
        return 0;
    }
    switch (how) {
    case SIG_BLOCK:   t->sig_blocked |= *set;  break;
    case SIG_UNBLOCK: t->sig_blocked &= ~*set; break;
    case SIG_SETMASK: t->sig_blocked = *set;   break;
    default:          return -EINVAL;
    }
    t->sig_blocked &= ~SIG_UNBLOCKABLE;
    return 0;
}

u32 signal_pending_set(void)
{
    /* POSIX's meaning: raised, and held back by the mask. */
    return current->sig_pending & current->sig_blocked;
}

/*
 * Sleep until something deliverable arrives. signal_send wakes a
 * blocked task whose queue is this one.
 */
static void wait_for_signal(void)
{
    struct waitq q;

    q.head = 0;
    while (!signal_pending(current)) {
        sleep_on(&q);
    }
}

int signal_pause(void)
{
    wait_for_signal();
    return -ERESTARTNOHAND;
}

int signal_suspend(u32 mask)
{
    struct task *t = current;

    /*
     * The mask in force while waiting is the one given; the old one
     * comes back when the handler returns, because it is what the
     * signal frame records -- see setup_frame.
     */
    t->sig_saved_mask = t->sig_blocked;
    t->sig_restore_mask = 1;
    t->sig_blocked = mask & ~SIG_UNBLOCKABLE;
    wait_for_signal();
    return -ERESTARTNOHAND;
}

/* --- the user stack pointer ------------------------------------------ */

/*
 * The 68040 keeps it in a register of its own, which is this task's
 * while this task is in the kernel -- switch_context carries it between
 * tasks -- so it is read and written directly.
 */
static u32 get_usp(void)
{
    u32 v;

    __asm__ volatile ("move.l %%usp,%0" : "=a"(v));
    return v;
}

static void set_usp(u32 v)
{
    __asm__ volatile ("move.l %0,%%usp" : : "a"(v));
}

/* --- restarting an interrupted call ---------------------------------- */

/*
 * A system call that a signal interrupted returns -EINTR, or, for the
 * two calls whose whole job is waiting for a signal, -ERESTARTNOHAND.
 * What the program sees depends on what happened to the signal:
 *
 *   no handler ran (it stopped and continued, or was ignored):
 *       the call is RESTARTED, invisibly -- a read interrupted by
 *       ctrl-Z and fg carries on reading, as it does on Linux;
 *   a handler ran, with SA_RESTART:
 *       restarted after the handler, unless it was pause/sigsuspend;
 *   a handler ran, without SA_RESTART:
 *       -EINTR.
 *
 * Restarting means putting the call's number back in d0 and the pc
 * back on the trap instruction (two bytes), so the same call runs again
 * with the same arguments, which are still in d1-d5 and a0.
 */
static void syscall_outcome(struct task *t, struct pt_regs *regs,
                            const struct sigaction *handler)
{
    s32 r;
    int restart = 0;

    if (t->syscall_nr < 0) {
        return;
    }
    r = (s32)regs->d[0];
    if (r == -ERESTARTNOHAND) {
        if (handler) {
            regs->d[0] = (u32)-EINTR;
        } else {
            restart = 1;
        }
    } else if (r == -EINTR) {
        restart = !handler || (handler->sa_flags & SA_RESTART);
    }
    if (restart) {
        regs->d[0] = (u32)t->syscall_nr;
        regs->pc -= 2;
    }
    t->syscall_nr = -1;         /* decided; an interrupt must not re-decide */
}

/* --- running a handler ----------------------------------------------- */

struct sigframe {
    u32 retaddr;                /* sa_restorer: where the handler returns */
    u32 sig;                    /* the handler's argument                 */
    u32 code;
    u32 scp;                    /* -> sc, for a handler that wants it      */
    struct sigcontext sc;
} __attribute__((packed));

#define SIGFRAME_SC_OFFSET  16

static int setup_frame(struct task *t, struct pt_regs *regs, int sig,
                       struct sigaction *act)
{
    static struct sigframe f;   /* 300 bytes; not on a 4 KB kernel stack.
                                 * One at a time: the kernel does not
                                 * preempt itself. */
    u32 usp = get_usp();
    u32 fp;
    unsigned fmt = (unsigned)(regs->format >> 12);

    /*
     * A frame can be redirected by changing its pc only if it is one
     * the rte will simply return through: format 0, or format 2, which
     * adds an address. The access-fault frame (7) re-runs the faulting
     * access, and faults end the program before reaching here anyway.
     */
    if (fmt != 0 && fmt != 2) {
        return -1;
    }

    syscall_outcome(t, regs, act);

    memset(&f, 0, sizeof(f));
    f.sc.sc_mask = t->sig_restore_mask ? t->sig_saved_mask : t->sig_blocked;
    t->sig_restore_mask = 0;
    f.sc.sc_usp = usp;
    memcpy(f.sc.sc_d, regs->d, sizeof(f.sc.sc_d));
    memcpy(f.sc.sc_a, regs->a, sizeof(f.sc.sc_a));
    f.sc.sc_sr = regs->sr;
    f.sc.sc_pc = regs->pc;
    f.sc.sc_format = regs->format;

    /* The FPU is this task's own right now: it is the one running. A
     * handler that uses floating point must not cost the interrupted
     * code its registers. */
    {
        u32 fpu[52];            /* aligned; the frame is packed */

        fpu_save(fpu);
        memcpy(f.sc.sc_fpu, fpu, sizeof(fpu));
    }

    fp = (usp - sizeof(f)) & ~3UL;
    f.retaddr = (u32)act->sa_restorer;
    f.sig = (u32)sig;
    f.code = 0;
    f.scp = fp + SIGFRAME_SC_OFFSET;

    if (copy_to_user(fp, &f, sizeof(f)) < 0) {
        return -1;              /* the stack is not usable */
    }

    set_usp(fp);
    regs->pc = (u32)act->sa_handler;
    regs->sr &= ~SR_TRACE;

    t->sig_blocked |= act->sa_mask;
    if (!(act->sa_flags & SA_NODEFER)) {
        t->sig_blocked |= SIGMASK(sig);
    }
    t->sig_blocked &= ~SIG_UNBLOCKABLE;
    if (act->sa_flags & SA_RESETHAND) {
        act->sa_handler = SIG_DFL;
    }
    return 0;
}

/* A program whose signal frame cannot be built or read back is beyond
 * helping, and SIGSEGV is what Linux gives it. */
static void force_segv(struct task *t)
{
    t->signalled = SIGSEGV;
    task_exit(128 + SIGSEGV);
}

s32 signal_return(struct pt_regs *regs)
{
    struct task *t = current;
    static struct sigcontext sc;
    u32 usp = get_usp();

    /* sigreturn itself is never restarted: its d0 is the one it put back. */
    t->syscall_nr = -1;

    /*
     * The trampoline runs after the handler's rts has popped the return
     * address, so the stack is at `sig`, and the context is 12 bytes up.
     */
    if (!t->as || copy_from_user(&sc, usp + 12, sizeof(sc)) < 0) {
        force_segv(t);
    }

    memcpy(regs->d, sc.sc_d, sizeof(regs->d));
    memcpy(regs->a, sc.sc_a, sizeof(regs->a));
    regs->pc = sc.sc_pc;

    /*
     * The condition codes only. Everything else in the saved sr is the
     * supervisor's -- the S bit above all, which a forged frame would
     * otherwise use to return to user code in supervisor mode.
     */
    regs->sr = (u16)((regs->sr & ~SR_CCR & ~SR_TRACE) | (sc.sc_sr & SR_CCR));
    set_usp(sc.sc_usp);
    t->sig_blocked = sc.sc_mask & ~SIG_UNBLOCKABLE;

    /* Only frames the kernel itself writes are restored. Under QEMU that
     * is always idle; a real 68040 could have saved a busy frame, which
     * this would reduce to idle -- losing an exception that was pending
     * when the signal arrived, rather than trusting one from user memory. */
    {
        u32 fpu[52];

        memcpy(fpu, sc.sc_fpu, sizeof(fpu));
        if (fpu[0] != 0 && fpu[0] != FPU_IDLE_FRAME) {
            fpu[0] = FPU_IDLE_FRAME;
        }
        fpu_restore(fpu);
    }

    return (s32)regs->d[0];
}

/* --- acting on what is pending --------------------------------------- */

static void stop_task(struct task *t, int sig)
{
    /*
     * Stopping is a state, not an unwind: the task stays exactly where
     * it is, and a later SIGCONT resumes it there. That is only
     * possible because it has a kernel stack of its own to be left
     * sitting on.
     */
    t->signalled = sig;
    t->state = TASK_STOPPED;
    t->stop_reported = 0;
    if (t->parent) {
        if (!(t->parent->sigact[SIGCHLD].sa_flags & SA_NOCLDSTOP)) {
            signal_send(t->parent, SIGCHLD);
        }
        wake_all(&t->parent->child_wait);
    }
    schedule();
    t->signalled = 0;
}

void signal_deliver(struct pt_regs *regs)
{
    struct task *t = current;
    int sig, handled = 0;

    if (!t || !t->as) {
        return;
    }

    while (!handled && (sig = next_signal(t)) != 0) {
        struct sigaction *act = &t->sigact[sig];

        t->sig_pending &= ~SIGMASK(sig);

        if (sig != SIGKILL && sig != SIGSTOP &&
            act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
            /* One handler per return to user mode. Anything else still
             * pending is taken on the way back from sigreturn. */
            if (setup_frame(t, regs, sig, act) < 0) {
                force_segv(t);
            }
            handled = 1;
            break;
        }
        if (act->sa_handler == SIG_IGN && sig != SIGKILL && sig != SIGSTOP) {
            continue;
        }

        switch (default_action(sig)) {
        case SIG_IGNORE:
            break;

        case SIG_CONT:
            /* The resuming was done when it was sent. */
            t->stop_reported = 0;
            break;

        case SIG_STOP:
            stop_task(t, sig);
            break;

        case SIG_TERM:
        default:
            t->signalled = sig;
            task_exit(128 + sig);       /* does not return */
        }
    }

    if (!handled) {
        syscall_outcome(t, regs, 0);
        if (t->sig_restore_mask) {
            t->sig_blocked = t->sig_saved_mask;
            t->sig_restore_mask = 0;
        }
    }
}
