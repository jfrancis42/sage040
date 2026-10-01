/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ptrace.c - one process watching and steering another.
 *
 * Linux's ptrace(2), as strace and gdb use it. A TRACER attaches to a
 * TRACEE -- or the tracee asks for it, PTRACE_TRACEME, before it execs
 * -- and from then on the tracee stops, and its tracer hears of it
 * through waitpid, at each of:
 *
 *   a signal about to be acted on    status (sig << 8) | 0x7f; the
 *                                    tracer resumes it with the signal
 *                                    to deliver instead, or none
 *   system call entry and exit       (PTRACE_SYSCALL) SIGTRAP, | 0x80
 *                                    with PTRACE_O_TRACESYSGOOD
 *   an exec, a fork, a clone         (PTRACE_O_TRACE*) SIGTRAP with the
 *                                    event in bits 16-23
 *   one instruction                  (PTRACE_SINGLESTEP) the 68040's
 *                                    trace bit, T1, and SIGTRAP
 *
 * WHILE IT IS STOPPED it is TASK_TRACED, inside the kernel, on its own
 * kernel stack, and its user registers are the struct pt_regs every
 * way into the kernel saves (ptregs.h) -- ptrace_regs points at them.
 * That is all reading and writing them needs: the tracer changes the
 * saved copy, and the tracee leaves with what it finds. The user stack
 * pointer is not in pt_regs; it is in the CPU's USP register, which a
 * switched-out task keeps on its kernel stack where switch_context put
 * it, and its FPU state is in task->fpu, saved at every switch.
 *
 * Nothing here runs unless something is traced: every hook below
 * starts by asking.
 */
#include "ptrace.h"
#include "ptregs.h"
#include "task.h"
#include "signal.h"
#include "sysint.h"
#include "uaccess.h"
#include "vm.h"
#include "cache.h"
#include "wait.h"
#include "errno.h"
#include "string.h"

#define SR_T1       0x8000      /* trace each instruction             */
#define SR_CCR      0x001f      /* all a program may change of SR     */

/* --- stopping and starting ------------------------------------------- */

/*
 * Stop for the tracer, and come back when it says. `status` is what its
 * waitpid will be told. Returns with the tracee running again, ready to
 * carry on from where it stopped.
 */
static void ptrace_stop(int status, struct pt_regs *regs)
{
    struct task *t = current;

    if (!t->tracer) {
        return;
    }
    t->ptrace_status = status;
    t->ptrace_reported = 0;
    t->ptrace_regs = regs;
    t->ptrace_sig = 0;
    t->state = TASK_TRACED;
    /* CLD_TRAPPED, as Linux's tracer is told: a SIGCHLD, and a wake for
     * the waitpid it is probably already in. */
    signal_send(t->tracer, SIGCHLD);
    wake_all(&t->tracer->child_wait);
    schedule();
    t->ptrace_regs = 0;
}

/* Start a stopped tracee again: with `sig` to deliver (signal stops
 * only), stopping next at what `flags` asks for. */
static void resume(struct task *t, int sig, int flags)
{
    t->ptrace = flags;
    t->ptrace_sig = sig;
    if (t->ptrace_regs) {
        if (flags & PT_STEP) {
            t->ptrace_regs->sr |= SR_T1;
        } else {
            t->ptrace_regs->sr &= (u16)~SR_T1;
        }
    }
    t->state = TASK_READY;
}

static void detach(struct task *t, int sig)
{
    t->tracer = 0;
    t->ptrace_opts = 0;
    if (t->state == TASK_TRACED) {
        resume(t, sig, 0);
    } else {
        t->ptrace = 0;
    }
}

/* --- the stops --------------------------------------------------------- */

static int sysgood(const struct task *t)
{
    return SIGTRAP | ((t->ptrace_opts & PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
}

/*
 * SYSTEM CALL ENTRY. The call number goes where PEEKUSER's ORIG_D0
 * reads it, and d0 says -ENOSYS, as Linux's does: a tracer that changes
 * ORIG_D0 to -1 skips the call, and d0 -- whatever the tracer leaves in
 * it -- is then its result. Returns nonzero for "skip".
 */
int ptrace_syscall_enter(struct pt_regs *regs)
{
    struct task *t = current;

    if (!t->tracer || !(t->ptrace & PT_SYSCALL)) {
        return PTRACE_SYSCALL_RUN;
    }
    t->ptrace_orig_d0 = (s32)regs->d[0];
    regs->d[0] = (u32)-ENOSYS;
    ptrace_stop((sysgood(t) << 8) | 0x7f, regs);
    if (t->ptrace_sig) {
        signal_send(t, t->ptrace_sig);
    }
    return t->ptrace_orig_d0 == -1 ? PTRACE_SYSCALL_SKIP : PTRACE_SYSCALL_STOPPED;
}

void ptrace_syscall_exit(struct pt_regs *regs)
{
    struct task *t = current;

    if (!t->tracer || !(t->ptrace & PT_SYSCALL)) {
        return;
    }
    ptrace_stop((sysgood(t) << 8) | 0x7f, regs);
    t->ptrace_orig_d0 = -1;
    if (t->ptrace_sig) {
        signal_send(t, t->ptrace_sig);
    }
}

/*
 * SIGNAL-DELIVERY STOP: `sig` is about to be acted on. The tracer sees
 * it, and resumes with the signal that is to be acted on instead --
 * the same, another, or 0 for none. Returns that.
 */
int ptrace_signal(int sig, struct pt_regs *regs)
{
    struct task *t = current;
    struct siginfo *si = &t->ptrace_si;

    if (!t->tracer || sig == SIGKILL) {
        return sig;
    }
    memset(si, 0, sizeof(*si));
    si->si_signo = sig;
    if (t->fault_sig == sig) {
        si->si_code = t->fault_code;
        si->_sifields._sigfault.si_addr = t->fault_addr;
    } else {
        si->si_code = SI_USER;
    }
    ptrace_stop((sig << 8) | 0x7f, regs);
    return t->ptrace_sig;
}

static void event_stop(int event, u32 msg, struct pt_regs *regs)
{
    current->ptrace_msg = msg;
    ptrace_stop(((SIGTRAP | (event << 8)) << 8) | 0x7f, regs);
}

/* After a successful execve: an event stop if asked for, or the
 * SIGTRAP a tracee has always got, which is how a tracer that started
 * the program with TRACEME sees it arrive. */
void ptrace_exec(struct pt_regs *regs)
{
    struct task *t = current;

    if (!t->tracer) {
        return;
    }
    if (t->ptrace_opts & PTRACE_O_TRACEEXEC) {
        event_stop(PTRACE_EVENT_EXEC, (u32)t->pid, regs);
    } else {
        signal_send(t, SIGTRAP);
    }
}

/*
 * A traced task made a child. If the tracer asked to follow this kind,
 * the child is traced too from its first instruction -- it starts with
 * a SIGSTOP the tracer sees as its first stop -- and the parent stops
 * with the event, the child's pid its message.
 */
void ptrace_fork(struct task *child, u32 clone_flags, int vfork,
                 struct pt_regs *regs)
{
    struct task *t = current;
    int event;
    u32 opt;

    if (!t->tracer || !child) {
        return;
    }
    if (vfork) {
        event = PTRACE_EVENT_VFORK, opt = PTRACE_O_TRACEVFORK;
    } else if (clone_flags == 0 || (clone_flags & 0xff) == SIGCHLD) {
        event = PTRACE_EVENT_FORK, opt = PTRACE_O_TRACEFORK;
    } else {
        event = PTRACE_EVENT_CLONE, opt = PTRACE_O_TRACECLONE;
    }
    if (!(t->ptrace_opts & opt)) {
        return;
    }
    child->tracer = t->tracer;
    child->ptrace_opts = t->ptrace_opts;
    child->ptrace = 0;
    signal_send(child, SIGSTOP);
    event_stop(event, (u32)child->pid, regs);
}

/*
 * PTRACE_EVENT_EXIT: a tracee that asked for it stops as it begins to
 * exit, its registers still there to read and its wait status the
 * event's message -- which is how strace prints `exit_group(0) = ?`
 * rather than leaving the call unfinished.
 */
void ptrace_exit_event(int status)
{
    struct task *t = current;

    /* Not for SIGKILL: nothing may keep a killed task from dying, and
     * a tracer that never answers would. */
    if (!t->tracer || !(t->ptrace_opts & PTRACE_O_TRACEEXIT) || !t->user_regs ||
        t->signalled == SIGKILL) {
        return;
    }
    event_stop(PTRACE_EVENT_EXIT, (u32)status, t->user_regs);
}

/* A task is going: its tracees go free, and its tracer hears. */
void ptrace_exiting(struct task *t)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        struct task *x = task_slot(i);

        if (x->state != TASK_UNUSED && x->tracer == t) {
            if (x->ptrace_opts & PTRACE_O_EXITKILL) {
                signal_send(x, SIGKILL);
            }
            detach(x, 0);
        }
    }
    if (t->tracer) {
        if (t->tracer != t->parent) {
            signal_send(t->tracer, SIGCHLD);
        }
        wake_all(&t->tracer->child_wait);
    }
}

int ptrace_wait_status(struct task *t, struct task *tracer, int *status)
{
    if (t->tracer != tracer) {
        return 0;
    }
    if (t->state == TASK_TRACED && !t->ptrace_reported) {
        t->ptrace_reported = 1;
        *status = t->ptrace_status;
        return 1;
    }
    return 0;
}

/* --- the tracee's memory ------------------------------------------------ */

/*
 * One byte of the tracee, read or written, as the tracee would see it --
 * and further: a debugger plants breakpoints in code, which is read-only
 * to the program. A write to an owned read-only page makes it writable
 * for the one byte -- which, if another address space shares the page,
 * first gives this one a copy (vm_protect) -- and read-only again. A
 * file's MAP_SHARED page is never written this way: it would reach the
 * file.
 */
static int tracee_byte(struct task *t, u32 va, u8 *b, int write)
{
    struct addrspace *as = t->as;
    u32 pa = vm_translate(as, va, write);
    int unprotect = 0;

    if (!pa && vm_fault(as, va, write) >= 0) {
        pa = vm_translate(as, va, write);
    }
    if (!pa && write && vm_page_prot(as, va) == 0 && !vm_shared_page(as, va)) {
        if (vm_protect(as, va, VM_WRITE) < 0) {
            return -EIO;
        }
        unprotect = 1;
        pa = vm_translate(as, va, 1);
    }
    if (pa) {
        if (write) {
            *(u8 *)pa = *b;
        } else {
            *b = *(u8 *)pa;
        }
    }
    if (unprotect) {
        vm_protect(as, va, 0);
    }
    return pa ? 0 : -EIO;
}

static int tracee_word(struct task *t, u32 addr, u32 *val, int write)
{
    u8 b[4];
    int i, err;

    if (write) {
        b[0] = (u8)(*val >> 24), b[1] = (u8)(*val >> 16);
        b[2] = (u8)(*val >> 8), b[3] = (u8)*val;
    }
    for (i = 0; i < 4; i++) {
        err = tracee_byte(t, addr + (u32)i, &b[i], write);
        if (err < 0) {
            return err;
        }
    }
    if (write) {
        cache_flush_all();      /* it may have been an instruction */
    } else {
        *val = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
    }
    return 0;
}

/*
 * Another task's memory, LEN bytes at ADDR, for /proc/PID/mem: the
 * same byte-at-a-time path PEEKDATA and POKEDATA take, so a write to
 * text makes the page private first. Returns the bytes moved -- fewer
 * than asked where the range runs into an unmapped page -- or -EIO if
 * not even the first could be.
 */
s32 tracee_access(struct task *t, u32 addr, u8 *buf, u32 len, int write)
{
    u32 i;

    for (i = 0; i < len; i++) {
        if (tracee_byte(t, addr + i, &buf[i], write) < 0) {
            break;
        }
    }
    if (write && i) {
        cache_flush_all();      /* it may have been an instruction */
    }
    return i ? (s32)i : -EIO;
}

/* --- the tracee's registers -------------------------------------------- */

/* Where a switched-out task's USP is: on its kernel stack, above the
 * eleven callee-saved registers switch_context pushed after it. */
static u32 *saved_usp(struct task *t)
{
    return (u32 *)(t->ksp + 44);
}

/*
 * The general registers by PEEKUSER number. pt_regs is packed -- the
 * hardware frame puts the PC on a word boundary after the SR -- so its
 * members are read and written in place rather than through pointers.
 * Returns 0, or -EIO for a number that is not one of these.
 */
static int gpr(struct task *t, int regno, u32 *val, int write)
{
    struct pt_regs *r = t->ptrace_regs;

    if (regno >= PT_D1 && regno < PT_A0) {
        if (write) r->d[regno - PT_D1 + 1] = *val; else *val = r->d[regno - PT_D1 + 1];
    } else if (regno >= PT_A0 && regno < PT_D0) {
        if (write) r->a[regno - PT_A0] = *val; else *val = r->a[regno - PT_A0];
    } else if (regno == PT_D0) {
        if (write) r->d[0] = *val; else *val = r->d[0];
    } else if (regno == PT_USP) {
        if (write) *saved_usp(t) = *val; else *val = *saved_usp(t);
    } else if (regno == PT_PC) {
        if (write) r->pc = *val; else *val = r->pc;
    } else {
        return -EIO;
    }
    return 0;
}

static int get_reg(struct task *t, int regno, u32 *val)
{
    if (regno == PT_ORIG_D0) {
        *val = (u32)t->ptrace_orig_d0;
        return 0;
    }
    if (regno == PT_SR) {
        *val = t->ptrace_regs->sr;
        return 0;
    }
    if (regno >= PT_FP0 && regno <= PT_FPIAR) {
        /* fp0-fp7 three longs each, then the control registers: the
         * layout of task->fpu past its 100-byte state frame. */
        *val = t->fpu[25 + regno - PT_FP0];
        return 0;
    }
    return gpr(t, regno, val, 0);
}

static int put_reg(struct task *t, int regno, u32 val)
{
    if (regno == PT_ORIG_D0) {
        t->ptrace_orig_d0 = (s32)val;
        return 0;
    }
    if (regno == PT_SR) {
        /* The condition codes and nothing else: the supervisor bit and
         * the interrupt mask are not a program's to set, nor the trace
         * bit, which is PTRACE_SINGLESTEP's. */
        t->ptrace_regs->sr = (u16)((t->ptrace_regs->sr & ~SR_CCR) | (val & SR_CCR));
        return 0;
    }
    if (regno >= PT_FP0 && regno <= PT_FPIAR) {
        if (t->fpu[0] == 0) {
            t->fpu[0] = 0x41000000UL;   /* an idle frame, or frestore
                                         * would not load the registers */
        }
        t->fpu[25 + regno - PT_FP0] = val;
        return 0;
    }
    return gpr(t, regno, &val, 1);
}

/* --- ptrace(2) ---------------------------------------------------------- */

/* Who may trace whom: root anybody, anybody else their own processes. */
static int may_trace(const struct task *t)
{
    return current->euid == 0 ||
           (t->uid == current->euid && t->euid == current->euid &&
            t->suid == current->euid);
}

/* The tracee `pid`, traced by the caller and stopped for it. */
static struct task *stopped_tracee(int pid)
{
    struct task *t = task_find(pid);

    if (!t || t->tracer != current || t->state != TASK_TRACED ||
        !t->ptrace_regs) {
        return 0;
    }
    return t;
}

s32 sys_ptrace(int req, int pid, u32 addr, u32 data)
{
    struct task *t;
    u32 val, buf[27];
    int i, err;

    if (req == PTRACE_TRACEME) {
        if (current->tracer || !current->parent || !current->parent->as) {
            return -EPERM;
        }
        current->tracer = current->parent;
        return 0;
    }

    if (req == PTRACE_ATTACH) {
        t = task_find(pid);
        if (!t || !t->as || t->tgid != t->pid) {
            return -ESRCH;
        }
        if (t == current || t->tracer || t->state == TASK_ZOMBIE) {
            return -EPERM;
        }
        if (!may_trace(t)) {
            return -EPERM;
        }
        t->tracer = current;
        t->ptrace_opts = 0;
        t->ptrace = 0;
        return signal_send(t, SIGSTOP);
    }

    if (req == PTRACE_KILL) {
        t = task_find(pid);
        if (!t || t->tracer != current) {
            return -ESRCH;
        }
        return signal_send(t, SIGKILL);
    }

    t = stopped_tracee(pid);
    if (!t) {
        return -ESRCH;
    }

    switch (req) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
        err = tracee_word(t, addr, &val, 0);
        return err < 0 ? err : sys_store(data, &val, 4);

    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
        return tracee_word(t, addr, &data, 1);

    case PTRACE_PEEKUSR:
        if (addr & 3) {
            return -EIO;
        }
        err = get_reg(t, (int)(addr >> 2), &val);
        return err < 0 ? err : sys_store(data, &val, 4);

    case PTRACE_POKEUSR:
        if (addr & 3) {
            return -EIO;
        }
        return put_reg(t, (int)(addr >> 2), data);

    case PTRACE_GETREGS:
        for (i = 0; i < PT_NREGS; i++) {
            get_reg(t, i, &buf[i]);
        }
        return sys_store(data, buf, PT_NREGS * 4);

    case PTRACE_SETREGS:
        err = copy_from_user(buf, data, PT_NREGS * 4);
        if (err < 0) {
            return err;
        }
        for (i = 0; i < PT_NREGS; i++) {
            put_reg(t, i, buf[i]);
        }
        return 0;

    case PTRACE_GETFPREGS:
        return sys_store(data, &t->fpu[25], 27 * 4);

    case PTRACE_SETFPREGS:
        err = copy_from_user(buf, data, 27 * 4);
        if (err < 0) {
            return err;
        }
        if (t->fpu[0] == 0) {
            t->fpu[0] = 0x41000000UL;
        }
        memcpy(&t->fpu[25], buf, 27 * 4);
        return 0;

    case PTRACE_SETOPTIONS:
        if (data & ~(u32)PTRACE_O_MASK) {
            return -EINVAL;
        }
        t->ptrace_opts = data;
        return 0;

    case PTRACE_GETEVENTMSG:
        return sys_store(data, &t->ptrace_msg, 4);

    case PTRACE_GETSIGINFO:
        return sys_store(data, &t->ptrace_si, sizeof(t->ptrace_si));

    case PTRACE_SETSIGINFO:
        return copy_from_user(&t->ptrace_si, data, sizeof(t->ptrace_si));

    case PTRACE_CONT:
    case PTRACE_SYSCALL:
    case PTRACE_SINGLESTEP:
    case PTRACE_DETACH:
        if (data >= NSIG) {
            return -EIO;
        }
        if (req == PTRACE_DETACH) {
            detach(t, (int)data);
        } else {
            resume(t, (int)data, req == PTRACE_SYSCALL ? PT_SYSCALL :
                                 req == PTRACE_SINGLESTEP ? PT_STEP : 0);
        }
        return 0;
    }
    return -EIO;
}
