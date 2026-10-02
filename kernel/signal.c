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
#include "ptrace.h"
#include "cache.h"
#include "sysint.h"
#include "poll.h"
#include "coredump.h"
#include "task.h"
#include "wait.h"
#include "ptregs.h"
#include "uaccess.h"
#include "console.h"
#include "errno.h"
#include "string.h"
#include "wb040.h"

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

static int send_src(struct task *t, int sig, const struct sigsrc *src);

int signal_send(struct task *t, int sig)
{
    return send_src(t, sig, 0);
}

int signal_may(const struct task *t, int sig)
{
    if (!current || current->euid == 0) {
        return 1;
    }
    if (current->uid == t->uid || current->uid == t->suid ||
        current->euid == t->uid || current->euid == t->suid) {
        return 1;
    }
    return sig == SIGCONT && t->sid == current->sid;
}

int signal_send_user(struct task *t, int sig, s32 code, u32 value)
{
    struct sigsrc src;

    if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE || !t->as) {
        return -ESRCH;
    }
    if (!signal_may(t, sig)) {
        return -EPERM;
    }
    if (sig == 0) {
        return 0;
    }
    src.code = code;
    src.pid = current ? current->tgid : 0;
    src.uid = current ? current->uid : 0;
    src.value = value;
    src.set = 1;
    return send_src(t, sig, &src);
}

static int send_src(struct task *t, int sig, const struct sigsrc *src)
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
    if (sig == SIGKILL && t->state == TASK_TRACED) {
        t->state = TASK_READY;          /* only SIGKILL beats the tracer */
    }
    if (sig == SIGCONT || sig == SIGKILL) {
        t->sig_pending &= ~STOP_SIGNALS;
        if (t->state == TASK_STOPPED) {
            t->state = TASK_READY;
            if (sig == SIGCONT) {
                t->continued = 1;       /* for waitpid(WCONTINUED) */
                if (t->parent) {
                    wake_all(&t->parent->child_wait);
                }
            }
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

    if (!(t->sig_pending & bit) && sig < (int)(sizeof(t->sig_src) /
                                               sizeof(t->sig_src[0]))) {
        if (src) {
            t->sig_src[sig] = *src;
        } else {
            t->sig_src[sig].set = 0;
        }
    }
    t->sig_pending |= bit;

    /* A signalfd reads blocked signals, so whoever waits on one has to
     * look again -- and a blocked signal wakes nothing else. */
    poll_wake();

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

int signal_group(int pgid, int sig)
{
    struct task *t;
    int i, found = 0;

    for (i = 0; (t = task_nth(i)) != 0; i++) {
        if (t->pgid == pgid && t->as && t->state != TASK_ZOMBIE) {
            found = 1;
            if (sig) {
                signal_send(t, sig);
            }
        }
    }
    return found ? 0 : -ESRCH;
}

/*
 * kill(2) to a group, or to everybody: each task the caller may signal
 * gets it. 0 if any did, -EPERM if there were tasks and the caller could
 * signal none of them, -ESRCH if there were none.
 */
static int kill_many(int pgid, int all, int sig)
{
    struct task *t;
    int i, found = 0, sent = 0;

    for (i = 0; (t = task_nth(i)) != 0; i++) {
        if (!t->as || t->state == TASK_ZOMBIE ||
            (all ? t == current : t->pgid != pgid)) {
            continue;
        }
        found = 1;
        if (signal_send_user(t, sig, SI_USER, 0) == 0) {
            sent = 1;
        }
    }
    return sent ? 0 : found ? -EPERM : -ESRCH;
}

int signal_kill(int pid, int sig)
{
    struct task *t;

    if (sig < 0 || sig >= NSIG) {
        return -EINVAL;
    }
    /*
     * Linux's forms: 0 is the caller's own group, -N is group N, and -1
     * is every task the caller may signal but itself.
     *
     * WHO MAY SIGNAL WHOM was never checked: any user could kill any
     * process, root's included, and `kill -1` from a nobody would have
     * ended the machine's every program. signal_may is Linux's rule.
     */
    if (pid == 0) {
        return kill_many(current->pgid, 0, sig);
    }
    if (pid == -1) {
        return kill_many(0, 1, sig);
    }
    if (pid < 0) {
        return kill_many(-pid, 0, sig);
    }

    t = task_find(pid);

    /*
     * A signal is sent to a PROCESS. task_find answers with the thread
     * group leader, because its pid is the tgid, and that is the thread
     * it is delivered to: this system does not choose among threads,
     * and a program that wants a particular one says so with tgkill.
     * CPython, which handles every signal on its main thread, wants
     * exactly this.
     *
     * SIGKILL is the exception, because it ends the process rather than
     * being handled by it: every thread gets it.
     */
    if (t && t->as && t->state != TASK_ZOMBIE && !signal_may(t, sig)) {
        return -EPERM;
    }
    if (t && sig == SIGKILL && t->tgid == t->pid) {
        struct task *o;
        int i;

        for (i = 0; (o = task_nth(i)) != 0; i++) {
            if (o != t && o->tgid == t->tgid && o->as &&
                o->state != TASK_ZOMBIE) {
                signal_send(o, SIGKILL);
            }
        }
    }

    if (t && !t->as && t->state != TASK_ZOMBIE) {
        return -EPERM;          /* see signal_send: it would do nothing */
    }
    if (sig == 0) {
        /* kill(pid, 0) asks whether the task exists, and sends nothing. */
        return (t && t->state != TASK_UNUSED && t->state != TASK_ZOMBIE)
               ? 0 : -ESRCH;
    }
    return signal_send_user(t, sig, SI_USER, 0);
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
    t->sigact[sig] = *act;
    t->sigact[sig].sa_mask &= ~SIG_UNBLOCKABLE;

    /*
     * A DISPOSITION BELONGS TO THE PROCESS, not to the thread that set
     * it: CLONE_SIGHAND, and POSIX. So every thread of this one gets it
     * too. The MASK is per thread and stays that way, which is the
     * split POSIX draws and what lets a program keep signals out of its
     * worker threads by blocking them there.
     */
    if (t->tgid != t->pid || task_group_count(t) > 1) {
        struct task *o;
        int i;

        for (i = 0; (o = task_nth(i)) != 0; i++) {
            if (o != t && o->tgid == t->tgid) {
                o->sigact[sig] = t->sigact[sig];
            }
        }
    }

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

/*
 * The temporary mask of ppoll, pselect and epoll_pwait: in force for the
 * wait only. If the wait was cut short by a signal, that signal is
 * delivered UNDER the temporary mask and the old one comes back when
 * its handler returns, exactly as for sigsuspend; otherwise the old one
 * comes straight back.
 */
int signal_temp_mask(u32 umask, u32 size, u32 *old)
{
    u32 set[2];
    int err;

    if (!umask) {
        return 0;
    }
    if (size != SIGSET_BYTES) {
        return -EINVAL;
    }
    if ((err = fetch(set, umask, sizeof(set))) < 0) {
        return err;
    }
    *old = current->sig_blocked;
    current->sig_blocked = set[0] & ~SIG_UNBLOCKABLE;
    return 1;
}

void signal_temp_done(int pushed, u32 old, int interrupted)
{
    struct task *t = current;

    if (!pushed) {
        return;
    }
    if (interrupted && signal_pending(t)) {
        t->sig_saved_mask = old;
        t->sig_restore_mask = 1;
    } else {
        t->sig_blocked = old;
    }
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
    u32 retaddr;                /* where the handler returns              */
    u32 sig;                    /* the handler's argument                 */
    u32 code;
    u32 scp;                    /* -> sc, for a handler that wants it      */
    struct sigcontext sc;
    u16 retcode[4];             /* the trampoline, if no restorer given   */
    struct fault_wb wb;         /* writes a fault left for sigreturn      */
} __attribute__((packed));

#define SIGFRAME_SC_OFFSET  16

/*
 * Linux/m68k's rt frame, exactly: what an SA_SIGINFO handler gets. The
 * handler is called with sig, pinfo and puc as its three arguments,
 * which are the three words above the return address.
 */
struct rt_sigframe {
    u32 pretcode;
    s32 sig;
    u32 pinfo;                  /* -> info */
    u32 puc;                    /* -> uc   */
    u16 retcode[4];
    struct siginfo info;
    struct ucontext uc;
};                              /* no padding: m68k aligns ints to two */

/*
 * Where an rt frame carries a fault's undone writes (wb040.c): in the
 * ucontext's filler, past the 100 bytes the FPU's state frame uses.
 * Linux keeps nothing there for m68k beyond the frame.
 */
#define UC_WB_OFFSET    240

/*
 * The fault this frame is for, if any: si_code and si_addr for the
 * handler, and the writes to carry. Taken once, by the frame for the
 * signal the fault raised.
 */
static int take_fault(struct task *t, int sig, struct fault_wb *wb)
{
    if (t->fault_sig != sig) {
        return 0;
    }
    t->fault_sig = 0;
    if (wb) {
        *wb = t->pending_wb;
    }
    memset(&t->pending_wb, 0, sizeof(t->pending_wb));
    return 1;
}


/*
 * Where a handler returns to. For the old frame with SA_RESTORER, the
 * address given -- lib/ulib's trampoline in crt0.s. Otherwise, and for
 * every rt frame, what Linux/m68k always does: two instructions written
 * into the frame, "move.l #nr,%d0" and "trap #0", which make the right
 * sigreturn call for the frame.
 *
 * Code written to memory has to be pushed out of the data cache and
 * invalidated in the instruction cache before it is run. The caches are
 * not turned on here -- CACR is never written -- so today there is
 * nothing to push; whoever enables them has this to do, and exec's
 * loading of program text as well.
 */
static void put_retcode(void *where, u32 nr)
{
    u16 code[4];

    code[0] = 0x203c;                   /* move.l #imm,%d0 */
    code[1] = (u16)(nr >> 16);
    code[2] = (u16)nr;
    code[3] = 0x4e40;                   /* trap #0 */
    memcpy(where, code, sizeof(code));
}

static void block_for_handler(struct task *t, int sig, struct sigaction *act);

/*
 * ALWAYS THE TRAMPOLINE, as for the rt frame, and as Linux/m68k does for
 * both: sa_restorer is accepted and ignored. musl relies on exactly
 * that -- it sets SA_RESTORER with an empty placeholder -- and honouring
 * it sent strace's SIGTERM handler "back" into an rts that popped
 * garbage: pc=0xf. The trampoline makes the same sigreturn call any
 * restorer here would.
 */
static u32 return_address(const struct sigaction *act, u32 retcode_uva)
{
    (void)act;
    return retcode_uva;
}

static int setup_rt_frame(struct task *t, struct pt_regs *regs, int sig,
                          struct sigaction *act, u32 usp);

/* Is `usp` on the task's alternate signal stack? */
static int on_altstack(const struct task *t, u32 usp)
{
    return t->ss_size && usp > t->ss_sp && usp - t->ss_sp <= t->ss_size;
}

/*
 * Where a handler's frame goes: below the interrupted stack pointer, or
 * at the top of the alternate stack for an SA_ONSTACK handler -- unless
 * the task is already on it, when a nested signal stacks up on it
 * normally. The frame records the interrupted pointer either way, so
 * sigreturn goes back to the stack the program was on.
 */
static u32 frame_base(const struct task *t, const struct sigaction *act, u32 usp)
{
    if ((act->sa_flags & SA_ONSTACK) && t->ss_size && !on_altstack(t, usp)) {
        return t->ss_sp + t->ss_size;
    }
    return usp;
}

int signal_altstack(const stack_t *ss, stack_t *old)
{
    struct task *t = current;
    int on = on_altstack(t, get_usp());

    if (old) {
        old->ss_sp = (void *)t->ss_sp;
        old->ss_size = t->ss_size;
        old->ss_flags = on ? SS_ONSTACK : (t->ss_size ? 0 : SS_DISABLE);
    }
    if (!ss) {
        return 0;
    }
    if (on) {
        return -EPERM;          /* not while running on it */
    }
    if (ss->ss_flags == SS_DISABLE) {
        t->ss_sp = t->ss_size = 0;
        return 0;
    }
    if (ss->ss_flags != 0 && ss->ss_flags != SS_ONSTACK) {
        return -EINVAL;
    }
    if (ss->ss_size < MINSIGSTKSZ) {
        return -ENOMEM;
    }
    t->ss_sp = (u32)ss->ss_sp;
    t->ss_size = ss->ss_size;
    return 0;
}

/*
 * Can a handler be entered from this exception frame? Only if the rte
 * would simply return through it -- format 0, format 2 (which adds an
 * address), or format 3, the 68040's floating-point post-instruction
 * frame, which is the same shape and is what a SIGFPE the FPSP reports
 * arrives on. Not format 7, the access-fault frame: its rte finishes
 * the faulting access, and changing its pc would lose that.
 */
static int frame_redirectable(const struct pt_regs *regs)
{
    unsigned fmt = (unsigned)(regs->format >> 12);

    return fmt == 0 || fmt == 2 || fmt == 3;
}

static int setup_frame(struct task *t, struct pt_regs *regs, int sig,
                       struct sigaction *act)
{
    static struct sigframe f;   /* 300 bytes; not on a 4 KB kernel stack.
                                 * One at a time: the kernel does not
                                 * preempt itself. */
    u32 usp = get_usp();
    u32 fp;

    if (!frame_redirectable(regs)) {
        return -1;              /* signal_deliver does not ask */
    }

    syscall_outcome(t, regs, act);

    if (act->sa_flags & SA_SIGINFO) {
        return setup_rt_frame(t, regs, sig, act, usp);
    }

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

    fp = (frame_base(t, act, usp) - sizeof(f)) & ~3UL;
    put_retcode((u8 *)&f + __builtin_offsetof(struct sigframe, retcode),
                __NR_sigreturn);
    f.retaddr = return_address(act, fp + (u32)__builtin_offsetof(struct sigframe,
                                                                 retcode));
    f.sig = (u32)sig;
    f.code = 0;
    f.scp = fp + SIGFRAME_SC_OFFSET;
    take_fault(t, sig, &f.wb);

    if (copy_to_user(fp, &f, sizeof(f)) < 0) {
        return -1;              /* the stack is not usable */
    }
    /* The trampoline is code written as data: out of the data cache and
     * out of the instruction cache before the handler returns into it.
     * QEMU has no cache to show this; the 68040 does. */
    cache_flush_all();

    set_usp(fp);
    regs->pc = (u32)act->sa_handler;
    regs->sr &= ~SR_TRACE;
    block_for_handler(t, sig, act);
    return 0;
}

static void block_for_handler(struct task *t, int sig, struct sigaction *act)
{
    t->sig_blocked |= act->sa_mask;
    if (!(act->sa_flags & SA_NODEFER)) {
        t->sig_blocked |= SIGMASK(sig);
    }
    t->sig_blocked &= ~SIG_UNBLOCKABLE;
    if (act->sa_flags & SA_RESETHAND) {
        act->sa_handler = SIG_DFL;
        act->sa_flags &= ~SA_SIGINFO;
    }
}

/*
 * The FPU part of a ucontext, both ways. task->fpu's layout (taskasm.s)
 * is the fsave frame in its first 100 bytes, fp0-fp7 at 100, and the
 * three control registers at 196; Linux/m68k's ucontext has the
 * registers in uc_mcontext.fpregs and the frame at the start of
 * uc_filler.
 */
static void fpu_to_uc(const u32 *fpu, struct ucontext *uc)
{
    memcpy(uc->uc_filler, fpu, 100);
    memcpy(uc->uc_mcontext.fpregs.f_fpregs, fpu + 25, 96);
    memcpy(uc->uc_mcontext.fpregs.f_fpcntl, fpu + 49, 12);
}

static void uc_to_fpu(const struct ucontext *uc, u32 *fpu)
{
    memcpy(fpu, uc->uc_filler, 100);
    memcpy(fpu + 25, uc->uc_mcontext.fpregs.f_fpregs, 96);
    memcpy(fpu + 49, uc->uc_mcontext.fpregs.f_fpcntl, 12);
}

static int setup_rt_frame(struct task *t, struct pt_regs *regs, int sig,
                          struct sigaction *act, u32 usp)
{
    static struct rt_sigframe f;    /* ~700 bytes: see setup_frame */
    u32 fpu[52];
    u32 fp;
    int i;

    memset(&f, 0, sizeof(f));
    fp = (frame_base(t, act, usp) - sizeof(f)) & ~3UL;

    f.sig = sig;
    f.pinfo = fp + (u32)((u8 *)&f.info - (u8 *)&f);
    f.puc = fp + (u32)((u8 *)&f.uc - (u8 *)&f);
    put_retcode(f.retcode, __NR_rt_sigreturn);
    /* Always the trampoline, whatever sa_restorer says: a restorer
     * written for the old frame (lib/ulib's is) would make the wrong
     * call, and Linux/m68k ignores sa_restorer for every frame. */
    f.pretcode = fp + (u32)__builtin_offsetof(struct rt_sigframe, retcode);

    f.info.si_signo = sig;
    f.info.si_code = SI_USER;
    if (sig < (int)(sizeof(t->sig_src) / sizeof(t->sig_src[0])) &&
        t->sig_src[sig].set) {
        const struct sigsrc *src = &t->sig_src[sig];

        f.info.si_code = src->code;
        f.info._sifields._kill.si_pid = src->pid;
        f.info._sifields._kill.si_uid = src->uid;
        f.info._sifields._pad[2] = (int)src->value;   /* _rt.si_value */
        t->sig_src[sig].set = 0;
    }
    {
        struct fault_wb wb;
        int code = t->fault_code;
        u32 addr = t->fault_addr;

        if (take_fault(t, sig, &wb)) {
            f.info.si_code = code;
            f.info._sifields._sigfault.si_addr = addr;
            memcpy((u8 *)f.uc.uc_filler + UC_WB_OFFSET, &wb, sizeof(wb));
        }
    }

    f.uc.uc_stack.ss_sp = (void *)t->ss_sp;
    f.uc.uc_stack.ss_size = t->ss_size;
    f.uc.uc_stack.ss_flags = !t->ss_size ? SS_DISABLE
                           : on_altstack(t, usp) ? SS_ONSTACK : 0;
    f.uc.uc_mcontext.version = MCONTEXT_VERSION;
    for (i = 0; i < 8; i++) {
        f.uc.uc_mcontext.gregs[i] = (int)regs->d[i];
    }
    for (i = 0; i < 7; i++) {
        f.uc.uc_mcontext.gregs[8 + i] = (int)regs->a[i];
    }
    f.uc.uc_mcontext.gregs[15] = (int)usp;
    f.uc.uc_mcontext.gregs[16] = (int)regs->pc;
    f.uc.uc_mcontext.gregs[17] = (int)regs->sr;
    f.uc.uc_sigmask[0] = t->sig_restore_mask ? t->sig_saved_mask
                                             : t->sig_blocked;
    t->sig_restore_mask = 0;

    fpu_save(fpu);
    fpu_to_uc(fpu, &f.uc);

    if (copy_to_user(fp, &f, sizeof(f)) < 0) {
        return -1;
    }
    cache_flush_all();          /* the trampoline: see setup_frame */

    set_usp(fp);
    regs->pc = (u32)act->sa_handler;
    regs->sr &= ~SR_TRACE;
    block_for_handler(t, sig, act);
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

    /*
     * Writes the fault left undone (wb040.c), now that the handler has
     * had its chance to make them possible. One that still cannot be
     * made is the fault again, and this time nothing is left to try.
     */
    {
        static struct fault_wb wb;

        if (copy_from_user(&wb, usp + 12 - SIGFRAME_SC_OFFSET +
                               (u32)__builtin_offsetof(struct sigframe, wb),
                           sizeof(wb)) < 0 ||
            wb040_redo(&wb) < 0) {
            force_segv(t);
        }
    }

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

/*
 * rt_sigreturn: back from an SA_SIGINFO handler. The handler's rts has
 * popped pretcode, so the frame starts four bytes below the stack
 * pointer. Everything comes from the ucontext, which the handler may
 * have changed -- that is what a ucontext is for -- with the same
 * limits as sigreturn: the condition codes and nothing else of the sr,
 * and an FPU frame the kernel could have written.
 */
s32 signal_rt_return(struct pt_regs *regs)
{
    struct task *t = current;
    static struct ucontext uc;
    u32 fpu[52];
    u32 frame = get_usp() - 4;
    int i;

    t->syscall_nr = -1;

    if (!t->as ||
        copy_from_user(&uc, frame + (u32)__builtin_offsetof(struct rt_sigframe, uc),
                       sizeof(uc)) < 0) {
        force_segv(t);
    }

    for (i = 0; i < 8; i++) {
        regs->d[i] = (u32)uc.uc_mcontext.gregs[i];
    }
    for (i = 0; i < 7; i++) {
        regs->a[i] = (u32)uc.uc_mcontext.gregs[8 + i];
    }
    regs->pc = (u32)uc.uc_mcontext.gregs[16];
    regs->sr = (u16)((regs->sr & ~SR_CCR & ~SR_TRACE) |
                     ((u16)uc.uc_mcontext.gregs[17] & SR_CCR));
    set_usp((u32)uc.uc_mcontext.gregs[15]);
    t->sig_blocked = uc.uc_sigmask[0] & ~SIG_UNBLOCKABLE;

    /* As in sigreturn: the writes a fault left, now possible or fatal. */
    {
        struct fault_wb wb;

        memcpy(&wb, (u8 *)uc.uc_filler + UC_WB_OFFSET, sizeof(wb));
        if (wb040_redo(&wb) < 0) {
            force_segv(t);
        }
    }

    uc_to_fpu(&uc, fpu);
    if (fpu[0] != 0 && fpu[0] != FPU_IDLE_FRAME) {
        fpu[0] = FPU_IDLE_FRAME;
    }
    fpu_restore(fpu);

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
    t->user_regs = regs;

    while (!handled && (sig = next_signal(t)) != 0) {
        struct sigaction *act = &t->sigact[sig];

        t->sig_pending &= ~SIGMASK(sig);

        /*
         * A TRACED task stops here for its tracer, which says what is to
         * be acted on in its place -- the same signal, another, or none
         * (ptrace.c). Not from a frame a handler cannot be started on:
         * the signal is left for later, as below, and the tracer told of
         * it then, once.
         */
        if (t->tracer && sig != SIGKILL) {
            if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN &&
                !frame_redirectable(regs)) {
                t->sig_pending |= SIGMASK(sig);
                return;
            }
            sig = ptrace_signal(sig, regs);
            if (!sig) {
                continue;
            }
            act = &t->sigact[sig];
        }

        if (sig != SIGKILL && sig != SIGSTOP &&
            act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
            /*
             * NOT NOW, from an access-fault frame. A fault that demand
             * paging or copy-on-write RESOLVED returns to the program
             * through here with its format 7 frame intact, and a signal
             * that arrived meanwhile -- the SIGCHLD of a child exiting
             * while the parent touched a page it had just forked -- used
             * to be refused a frame and turned into a SIGSEGV: bash
             * died on every mistyped command, at the machine's real
             * speed, where the timing is the same every time. The
             * signal stays pending and is delivered on the next return
             * to user mode, which is the next system call or tick.
             */
            if (!frame_redirectable(regs)) {
                t->sig_pending |= SIGMASK(sig);
                return;
            }
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
            /* d[8] then a[7]: pt_regs is packed, so they are the fifteen
             * consecutive words core_dump wants. */
            t->core_dumped = core_dump(t, sig, (const u32 *)(const void *)regs,
                                       regs->sr, regs->pc,
                                       regs->format);
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
