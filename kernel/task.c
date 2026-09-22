/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * task.c - the scheduler.
 *
 * Round robin over whatever is ready, with a fixed slice. There is no
 * priority and no fairness accounting, and both are deliberate: a
 * machine running a shell, a program and an idle loop gets nothing from
 * a scheduler that can rank them, and a wrong ranking is far harder to
 * see than a simple one. When there is something to prioritise, this is
 * the file that grows.
 */
#include "task.h"
#include "vm.h"
#include "pmm.h"
#include "wait.h"
#include "vfs.h"
#include "signal.h"
#include "ptregs.h"
#include "tty.h"
#include "timer.h"
#include "console.h"
#include "errno.h"
#include "string.h"

extern void switch_context(u32 *save_sp, u32 new_sp);
extern void fpu_save(u32 *area);
extern void fpu_restore(const u32 *area);

/* A 68040 idle state frame: version 0x41, no further state. */
#define FPU_IDLE_FRAME  0x41000000UL
extern void task_entry(void);

struct task *current;

static struct task tasks[TASK_MAX];
static struct task *idle;
static int next_pid = 1;

/*
 * Set when the tick decides the running task has had its turn. Acted on
 * at the next return to user mode, not where it is set -- the tick runs
 * in an interrupt, and switching from there would mean switching out of
 * whatever the kernel was in the middle of.
 */
static volatile int need_resched;

/* --- the kernel stack ------------------------------------------------ */

/*
 * Three pages: a guard and then two of stack.
 *
 * The guard comes out of the KERNEL's map rather than the task's,
 * because it is the kernel that would overflow -- running that task's
 * system calls. A kernel stack that runs past its end and keeps going
 * corrupts whatever is next and surfaces somewhere else entirely, which
 * is close to the worst failure a system can have.
 */
#define KSTACK_PAGES    2
#define KSTACK_TOTAL    (KSTACK_PAGES + 1)
#define KSTACK_BYTES    (KSTACK_PAGES * (u32)PAGE_SIZE)

/*
 * THE HIGH-WATER MARK. Every kernel stack is painted with a pattern when
 * it is made, and when its task is reaped the paint left at the bottom
 * says how deep it ever went. The deepest of all is kept, with the name
 * of the task that reached it, and kstat(KSTAT_STACK) reports it -- so
 * "is 8 KB enough" is a measurement, not an estimate from one frame.
 */
#define KSTACK_PAINT    0x5ac3a55aUL

static u32  kstack_max;
static char kstack_max_name[TASK_NAME_MAX];

static u32 kstack_alloc(u32 *top)
{
    u32 base = pmm_alloc_pages(KSTACK_TOTAL);
    u32 *p, *end;

    if (!base) {
        return 0;
    }
    vm_kernel_present(base, 0);
    *top = base + KSTACK_TOTAL * (u32)PAGE_SIZE;
    for (p = (u32 *)(base + PAGE_SIZE), end = (u32 *)*top; p < end; p++) {
        *p = KSTACK_PAINT;
    }
    return base;
}

/* Bytes of a kernel stack ever used: from the top down to the deepest
 * word that is not paint any more. */
static u32 kstack_used(u32 base)
{
    const u32 *p = (const u32 *)(base + PAGE_SIZE);
    const u32 *end = (const u32 *)(base + KSTACK_TOTAL * (u32)PAGE_SIZE);

    while (p < end && *p == KSTACK_PAINT) {
        p++;
    }
    return (u32)((const u8 *)end - (const u8 *)p);
}

static void kstack_note(const struct task *t)
{
    u32 used = t->kstack ? kstack_used(t->kstack) : 0;

    if (used > kstack_max) {
        kstack_max = used;
        strncpy(kstack_max_name, t->name, sizeof(kstack_max_name) - 1);
    }
}

void task_kstack_stats(u32 *size, u32 *max_used, char *name, u32 namelen)
{
    int i;

    /* The live ones too: a task still running may be the deepest. */
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].kstack) {
            kstack_note(&tasks[i]);
        }
    }
    *size = KSTACK_BYTES;
    *max_used = kstack_max;
    strncpy(name, kstack_max_name, namelen - 1);
    name[namelen - 1] = '\0';
}

static void kstack_free(u32 base)
{
    if (base) {
        vm_kernel_present(base, 1);
        pmm_free_pages(base, KSTACK_TOTAL);
    }
}

/* --- making one ------------------------------------------------------ */

static struct task *alloc_task(const char *name)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            struct task *t = &tasks[i];

            memset(t, 0, sizeof(*t));
            t->pid = next_pid++;
            strncpy(t->name, name, TASK_NAME_MAX - 1);
            t->name[TASK_NAME_MAX - 1] = '\0';
            t->slice = TASK_SLICE;
            t->fpu[0] = FPU_IDLE_FRAME;
            t->syscall_nr = -1;
            /* The root, until somebody inherits or chdirs. */
            t->cwd_ino = 0;
            strcpy(t->cwd_path, "/");
            return t;
        }
    }
    return 0;
}

/*
 * The working directory is inherited the same way descriptors are, and
 * for the same reason: a child starts where its parent was standing.
 * A shell that spawns `ls` in /bin and gets a listing of / would be
 * reporting on a directory nobody asked about.
 */
void task_cwd_inherit(struct task *t, struct task *from)
{
    if (!t) {
        return;
    }
    if (!from) {
        t->cwd_ino = 0;
        strcpy(t->cwd_path, "/");
        t->umask = 022;
        return;
    }
    t->cwd_ino = from->cwd_ino;
    memcpy(t->cwd_path, from->cwd_path, sizeof(t->cwd_path));
    t->umask = from->umask;     /* inherited the same way, and as often */
}

/*
 * Build a kernel stack that looks like one belonging to a task suspended
 * in switch_context, so that the first schedule to it simply returns.
 *
 * From the top down: the exception frame it will RTE through, the
 * registers task_entry pops, then the callee-saved set and the return
 * address switch_context expects. Every layer here is read by code that
 * does not know it was fabricated, which is exactly the property wanted.
 */
static void build_stack(struct task *t, u32 top, u32 entry, u32 usp,
                        int user_mode)
{
    u32 sp = top;
    u16 *w;
    u32 *l;

    /* The exception frame: format 0, and an SR that decides the mode. */
    sp -= 8;
    w = (u16 *)sp;
    w[0] = (u16)(user_mode ? 0x0000 : 0x2000);  /* SR */
    l = (u32 *)(sp + 2);
    *l = entry;                                  /* PC */
    w = (u16 *)(sp + 6);
    w[0] = 0;                                    /* format 0, vector 0 */

    /* d0-d7/a0-a6, which task_entry pops. Zero: a program has no right
     * to expect anything in a register it has not set. */
    sp -= 60;
    memset((void *)sp, 0, 60);

    /* The return address switch_context will rts to. */
    sp -= 4;
    *(u32 *)sp = (u32)task_entry;

    /* USP, then the callee-saved registers. */
    sp -= 4;
    *(u32 *)sp = usp;
    sp -= 44;
    memset((void *)sp, 0, 44);

    t->ksp = sp;
}

struct task *task_create(const char *name, void (*entry)(void))
{
    struct task *t = alloc_task(name);
    u32 top;

    if (!t) {
        return 0;
    }
    t->kstack = kstack_alloc(&top);
    if (!t->kstack) {
        t->state = TASK_UNUSED;
        return 0;
    }
    t->pgid = t->pid;           /* a kernel task leads its own group */

    /* A kernel task: supervisor mode, and its "user" stack pointer is
     * never used because it never goes there. */
    build_stack(t, top, (u32)entry, 0, 0);
    strncpy(t->cmd, name, JOB_CMD_MAX - 1);

    /*
     * Descriptors are inherited, here as much as for a program.
     *
     * The console is bound to 0, 1 and 2 of whatever task exists when
     * the terminal comes up -- which is the boot task -- so a shell
     * created without inheriting them has nowhere to print. It ran
     * perfectly and silently, which took a while to recognise as the
     * absence of a stdout rather than a broken context switch.
     */
    t->parent = current;
    fd_inherit(t, current);
    task_cwd_inherit(t, current);

    /*
     * A kernel task takes no signals at all (see signal_send), which is
     * what keeps the shell alive through the key that is meant to
     * interrupt what it is running.
     */

    t->state = TASK_READY;
    return t;
}

/*
 * Made by exec.c, which has already built an address space and a user
 * stack. Everything else is the same.
 */
struct task *task_create_user(const char *name, u32 entry, u32 usp,
                              struct addrspace *as)
{
    struct task *t = alloc_task(name);
    u32 top;

    if (!t) {
        return 0;
    }
    t->kstack = kstack_alloc(&top);
    if (!t->kstack) {
        t->state = TASK_UNUSED;
        return 0;
    }
    t->as = as;
    build_stack(t, top, entry, usp, 1);
    t->stop_reported = 0;
    t->state = TASK_READY;
    return t;
}

/*
 * fork(): a copy of the current task, returning from the same system
 * call with 0 where the parent gets the child's pid.
 *
 * The child's kernel stack is built like any new task's -- which is to
 * say, as struct pt_regs and a format 0 frame, what every return to user
 * mode pops -- and then the parent's registers are copied into it. So the
 * child's first rte lands on the instruction after the parent's trap,
 * with everything the parent had except d0.
 */
struct task *task_fork(struct pt_regs *regs)
{
    struct task *p = current;
    struct task *t;
    u32 top, usp;
    struct pt_regs *cr;

    if (!p->as) {
        return 0;               /* a kernel task has nothing to copy */
    }
    t = alloc_task(p->name);
    if (!t) {
        return 0;
    }
    t->kstack = kstack_alloc(&top);
    if (!t->kstack) {
        t->state = TASK_UNUSED;
        return 0;
    }
    t->as = vm_clone(p->as);
    if (!t->as) {
        kstack_free(t->kstack);
        t->state = TASK_UNUSED;
        return 0;
    }

    __asm__ volatile ("move.l %%usp,%0" : "=a"(usp));
    build_stack(t, top, regs->pc, usp, 1);
    cr = (struct pt_regs *)(top - sizeof(struct pt_regs));
    memcpy(cr->d, regs->d, sizeof(cr->d));
    memcpy(cr->a, regs->a, sizeof(cr->a));
    cr->d[0] = 0;               /* fork() returns 0 in the child */
    cr->sr = regs->sr;
    cr->pc = regs->pc;
    cr->format = 0;

    /* What a forked child inherits, as POSIX lists it. */
    memcpy(t->cmd, p->cmd, sizeof(t->cmd));
    t->parent = p;
    t->pgid = p->pgid;
    fd_fork(t, p);
    task_cwd_inherit(t, p);
    memcpy(t->sigact, p->sigact, sizeof(t->sigact));
    t->sig_blocked = p->sig_blocked;
    /* Pending signals, timers and times are the parent's own: not copied. */

    /* The FPU is live in the parent right now; the child starts with
     * what it holds. */
    fpu_save(p->fpu);
    memcpy(t->fpu, p->fpu, sizeof(t->fpu));

    t->state = TASK_READY;
    return t;
}

/* --- the table ------------------------------------------------------- */

struct task *task_find(int pid)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) {
            return &tasks[i];
        }
    }
    return 0;
}

struct task *task_nth(int index)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && index-- == 0) {
            return &tasks[i];
        }
    }
    return 0;
}

int task_count(void)
{
    int i, n = 0;

    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            n++;
        }
    }
    return n;
}

const char *task_state_name(int state)
{
    switch (state) {
    case TASK_READY:   return "ready";
    case TASK_RUNNING: return "running";
    case TASK_BLOCKED: return "blocked";
    case TASK_STOPPED: return "stopped";
    case TASK_ZOMBIE:  return "done";
    default:           return "?";
    }
}

/* --- choosing -------------------------------------------------------- */

/*
 * The next ready task after this one, wrapping.
 *
 * Starting from the one after the current task rather than from the top
 * is the whole of the round robin: it is what stops the first ready task
 * in the table from being chosen every time and starving everything
 * behind it.
 */
static struct task *pick_next(void)
{
    int start = (int)(current - tasks);
    int i;

    for (i = 1; i <= TASK_MAX; i++) {
        struct task *t = &tasks[(start + i) % TASK_MAX];

        /*
         * The idle task is skipped here and returned below only when
         * there is nothing else. An idle task that competed for the
         * processor on equal terms would take every other turn from
         * whatever was actually working, which is not idling.
         */
        if (t != idle && t->state == TASK_READY) {
            return t;
        }
    }
    if (current->state == TASK_RUNNING) {
        return current;         /* nothing else wants it */
    }
    return idle;
}

/*
 * Zombies that nobody will ever wait for, because their parent has gone.
 * Not the current task: a task cannot free the kernel stack it is
 * standing on, so a zombie is always reaped by somebody else.
 */
static void reap_orphans(void)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];

        if (t->state == TASK_ZOMBIE && !t->parent && t != current) {
            task_reap(t);
        }
    }
}

void schedule(void)
{
    struct task *prev = current;
    struct task *next;

    reap_orphans();

    need_resched = 0;

    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }

    next = pick_next();
    next->state = TASK_RUNNING;
    next->slice = TASK_SLICE;
    current = next;

    if (next == prev) {
        return;                 /* nothing to switch to */
    }

    /*
     * The address space goes with the task. A kernel task has none, and
     * runs with whatever was loaded -- it cannot reach user memory
     * anyway, because its accesses are supervisor accesses and walk the
     * kernel's map.
     */
    if (next->as != prev->as) {
        vm_switch(next->as);
    }

    /*
     * The FPU goes with the task too. Nothing in the kernel uses it, so
     * it holds exactly what `prev` left there, and it can be swapped
     * here, outside switch_context, with no risk of the kernel's own
     * state being caught in the middle.
     */
    fpu_save(prev->fpu);
    fpu_restore(next->fpu);

    switch_context(&prev->ksp, next->ksp);

    /*
     * Reached when somebody schedules back to `prev`, which by then is
     * the current task again. Anything after this line runs in the
     * context that called schedule(), not the one that was switched to.
     */
}

/*
 * Wake anything whose sleep has run out of time.
 *
 * From the tick, so a sleeping task does not depend on anything else
 * happening. Without this a sleep with a timeout would only ever end if
 * the thing it was waiting for arrived, which is the opposite of what a
 * timeout is for.
 */
void task_timeouts(void)
{
    u32 now = timer_jiffies();
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];

        if (t->state == TASK_BLOCKED && t->wake_at &&
            (s32)(now - t->wake_at) >= 0) {
            if (t->queue) {
                /* Off the queue by hand: it was not woken, its time
                 * simply came, and `woken` must stay clear so the
                 * sleeper can tell the difference. */
                struct task *p = t->queue->head, *prev = 0;

                while (p) {
                    if (p == t) {
                        if (prev) {
                            prev->wait_next = t->wait_next;
                        } else {
                            t->queue->head = t->wait_next;
                        }
                        break;
                    }
                    prev = p;
                    p = p->wait_next;
                }
                t->queue = 0;
                t->wait_next = 0;
            }
            t->wake_at = 0;
            t->state = TASK_READY;
        }

        /* ITIMER_REAL: wall time, whether the task is running or not. */
        if (t->it_real_at && t->state != TASK_UNUSED &&
            t->state != TASK_ZOMBIE && (s32)(now - t->it_real_at) >= 0) {
            if (t->it_real_interval) {
                t->it_real_at += t->it_real_interval;
                /* Fallen behind -- the machine was busy, or the interval
                 * is shorter than a tick could keep up with: one signal,
                 * not a burst of them, and the next one a period on. */
                if ((s32)(now - t->it_real_at) >= 0) {
                    t->it_real_at = now + t->it_real_interval;
                }
            } else {
                t->it_real_at = 0;
            }
            signal_send(t, SIGALRM);
        }
    }
}

void task_tick(void)
{
    struct task *t = current;
    int user;

    if (!t) {
        return;
    }

    /*
     * Whose time this tick was. The interrupt's saved registers say
     * whether it landed in user code or in the kernel, which is all the
     * difference between user and system time.
     */
    user = irq_regs && pt_user_mode(irq_regs);
    if (user) {
        t->utime++;
    } else {
        t->stime++;
    }
    if (t->as) {
        if (user && t->it_virt && --t->it_virt == 0) {
            t->it_virt = t->it_virt_interval;
            signal_send(t, SIGVTALRM);
        }
        if (t->it_prof && --t->it_prof == 0) {
            t->it_prof = t->it_prof_interval;
            signal_send(t, SIGPROF);
        }
    }

    if (current->slice > 0) {
        current->slice--;
    }
    if (current->slice == 0) {
        need_resched = 1;
    }
}

/*
 * The one place a preemption actually happens.
 *
 * Called from the tail of an interrupt stub and from the end of a system
 * call, with the SR the interrupted context will return to. If that SR
 * says supervisor, the kernel is returning into itself and this does
 * nothing -- preempting there would mean switching out of whatever the
 * kernel was in the middle of, and the whole design avoids needing to
 * care about that.
 */
void task_ret_to_user(struct pt_regs *regs)
{
    if (!current || !pt_user_mode(regs)) {
        return;
    }

    /* Signals first: one may end the task, in which case there is
     * nothing to schedule it back to. */
    signal_deliver(regs);

    if (need_resched) {
        schedule();
    }
}

/* Called from task_entry, once, as a new task starts. */
void task_entry_hook(void)
{
    /* Nothing yet. It exists because a new task arrives in the kernel
     * having never been through a system call, and that is exactly the
     * moment anything per-task and lazily set up would want. */
}

/* --- ending ---------------------------------------------------------- */

void task_exit(int status)
{
    struct task *t = current;
    int i;

    t->exit_status = status;
    t->exiting = 1;

    /* Its descriptors go now; its kernel stack cannot, because this is
     * still standing on it. */
    for (i = 0; i < OPEN_MAX; i++) {
        if (t->fds[i]) {
            file_put(t->fds[i]);
            t->fds[i] = 0;
        }
    }

    /*
     * Its address space goes now too, as Linux's does at exit. It used
     * to wait for the reap, so a zombie nobody had waited for yet held
     * every page it ever had -- and, once there was swap, every slot:
     * swapoff met a process that had finished long before and still
     * had pages out. Nothing here runs on the user address space; the
     * kernel's own map is switched to first, so the tables being freed
     * are not the ones the MMU is looking at.
     */
    if (t->as) {
        vm_switch(0);
        vm_destroy(t->as);
        t->as = 0;
    }

    /*
     * Its children are orphans now. Linux gives them to init, which
     * waits for them; there is no init here, so an orphan that finishes
     * is reaped by the scheduler instead (see reap_orphans) -- otherwise
     * every child a program did not wait for would hold a task slot for
     * as long as the machine ran.
     */
    for (i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].parent == t) {
            tasks[i].parent = 0;
        }
    }

    /*
     * A zombie rather than gone: whoever started it is entitled to its
     * exit status, and the shell prints it. task_reap() finishes the
     * job once somebody has collected it.
     */
    t->state = TASK_ZOMBIE;

    /* Anything waiting for a child to finish wants to know -- by the
     * wait queue, and by SIGCHLD, which is discarded unless the parent
     * has a handler for it. */
    if (t->parent) {
        signal_send(t->parent, SIGCHLD);
        wake_all(&t->parent->child_wait);
    }

    schedule();
    /* Never reached: a zombie is never chosen. */
    for (;;) {
    }
}

void task_reap(struct task *t)
{
    if (!t || t->state != TASK_ZOMBIE) {
        return;
    }
    /* Its time becomes its parent's children's time, now that it has
     * been waited for. */
    if (t->parent) {
        t->parent->cutime += t->utime + t->cutime;
        t->parent->cstime += t->stime + t->cstime;
    }
    if (t->as) {
        vm_destroy(t->as);
        t->as = 0;
    }
    kstack_note(t);
    kstack_free(t->kstack);
    t->kstack = 0;
    t->state = TASK_UNUSED;
}

/* Is `t` one of the children a waitpid(pid) is asking about? */
static int wait_matches(struct task *t, int pid)
{
    if (t->state == TASK_UNUSED || t->parent != current) {
        return 0;
    }
    if (pid > 0) {
        return t->pid == pid;
    }
    if (pid == -1) {
        return 1;
    }
    if (pid == 0) {
        return t->pgid == current->pgid;
    }
    return t->pgid == -pid;
}

int task_wait(int pid, int *status, int options)
{
    for (;;) {
        int i, children = 0;

        for (i = 0; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];

            if (!wait_matches(t, pid)) {
                continue;
            }
            children++;

            /* Linux's status words: the exit code in the second byte,
             * or the signal in the low seven bits, or 0x7f and the
             * signal that stopped it. */
            if (t->state == TASK_ZOMBIE) {
                int got = t->pid;

                if (status) {
                    *status = t->signalled ? t->signalled
                                           : (t->exit_status & 0xff) << 8;
                }
                current->waited_utime = t->utime + t->cutime;
                current->waited_stime = t->stime + t->cstime;
                task_reap(t);
                return got;
            }

            /*
             * A child that has STOPPED is reported, once, if asked for
             * with WUNTRACED -- which a shell always asks for, or it
             * would wait for ever for a job that ctrl-Z put to one side.
             */
            if (t->state == TASK_STOPPED && !t->stop_reported &&
                (options & WUNTRACED)) {
                t->stop_reported = 1;
                if (status) {
                    *status = ((t->signalled ? t->signalled : SIGSTOP) << 8)
                              | 0x7f;
                }
                return t->pid;
            }
            if (t->continued && (options & WCONTINUED)) {
                t->continued = 0;
                if (status) {
                    *status = 0xffff;
                }
                return t->pid;
            }
        }
        if (!children) {
            return -ECHILD;
        }
        if (options & WNOHANG) {
            return 0;
        }
        /* Interruptible, as waitpid is everywhere: a handler runs, and
         * the wait resumes after it only with SA_RESTART. */
        if (signal_pending(current)) {
            return -EINTR;
        }
        sleep_on(&current->child_wait);
    }
}

/* --- starting up ------------------------------------------------------ */

void task_init(void)
{
    int i;

    for (i = 0; i < TASK_MAX; i++) {
        tasks[i].state = TASK_UNUSED;
    }

    /*
     * The kernel's own startup becomes task 1, and becomes the idle
     * task: it is already running on a stack, with registers, and the
     * first switch away from it saves its context wherever it happens to
     * be. Nothing has to be fabricated for a task that has always
     * existed -- and having the boot path turn into the idle loop saves
     * both a kernel stack and a special case for "the task that was
     * here before there were tasks".
     */
    current = &tasks[0];
    memset(current, 0, sizeof(*current));
    current->pid = next_pid++;
    current->state = TASK_RUNNING;
    current->slice = TASK_SLICE;
    current->fpu[0] = FPU_IDLE_FRAME;
    current->syscall_nr = -1;
    strcpy(current->name, "idle");
    strcpy(current->cmd, "idle");
    /* The root. Task 0 is built by hand rather than through
     * alloc_task(), so it has to be said here too -- and everything
     * else inherits from it, so an empty string here is an empty
     * working directory for every task the machine ever runs. */
    current->cwd_ino = 0;
    strcpy(current->cwd_path, "/");
    current->pgid = current->pid;
    idle = current;
}

int task_can_sleep(void)
{
    return current && current != idle && !irq_regs && dev_timer() != 0;
}
