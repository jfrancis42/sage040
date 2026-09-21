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
#include "tty.h"
#include "timer.h"
#include "console.h"
#include "errno.h"
#include "string.h"

extern void switch_context(u32 *save_sp, u32 new_sp);
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

static u32 kstack_alloc(u32 *top)
{
    u32 base = pmm_alloc_pages(KSTACK_TOTAL);

    if (!base) {
        return 0;
    }
    vm_kernel_present(base, 0);
    *top = base + KSTACK_TOTAL * (u32)PAGE_SIZE;
    return base;
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
            return t;
        }
    }
    return 0;
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

    /*
     * A kernel task is the shell or something like it, and a shell must
     * not die of the key that is meant to interrupt what it is running.
     * Ignoring rather than blocking, so that a stray one is discarded
     * instead of arriving later at a confusing moment.
     */
    t->sig_ignored = SIGMASK(SIGINT) | SIGMASK(SIGTSTP);

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

void schedule(void)
{
    struct task *prev = current;
    struct task *next;

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
    }
}

void task_tick(void)
{
    if (!current) {
        return;
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
void task_ret_to_user(u32 saved_sr)
{
    if (!current || (saved_sr & 0x2000)) {
        return;
    }

    /* Signals first: one may end the task, in which case there is
     * nothing to schedule it back to. */
    signal_deliver();

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

    /* Its descriptors go now; its stack and address space cannot, because
     * this is still standing on one of them. */
    for (i = 0; i < OPEN_MAX; i++) {
        if (t->fds[i]) {
            file_put(t->fds[i]);
            t->fds[i] = 0;
        }
    }

    /*
     * A zombie rather than gone: whoever started it is entitled to its
     * exit status, and the shell prints it. task_reap() finishes the
     * job once somebody has collected it.
     */
    t->state = TASK_ZOMBIE;

    /* Anything waiting for a child to finish wants to know. */
    if (t->parent) {
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
    if (t->as) {
        vm_destroy(t->as);
        t->as = 0;
    }
    kstack_free(t->kstack);
    t->kstack = 0;
    t->state = TASK_UNUSED;
}

int task_wait(int pid, int *status)
{
    for (;;) {
        int i, children = 0;

        for (i = 0; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];

            if (t->state == TASK_UNUSED || t->parent != current) {
                continue;
            }
            if (pid > 0 && t->pid != pid) {
                continue;
            }
            children++;

            if (t->state == TASK_ZOMBIE) {
                int got = t->pid;

                if (status) {
                    *status = t->signalled ? 128 + t->signalled
                                           : t->exit_status;
                }
                task_reap(t);
                return got;
            }

            /*
             * A child that has STOPPED is reported too, once.
             *
             * Without this the shell waits forever for a task that is
             * never going to finish, because ctrl-Z did not end it -- it
             * put it to one side. Real systems make this optional, with
             * WUNTRACED; here a shell is the only caller and it always
             * wants to know.
             *
             * Reported once, because the child stays stopped and would
             * otherwise be reported again on the next wait.
             */
            if (t->state == TASK_STOPPED && !t->stop_reported) {
                t->stop_reported = 1;
                if (status) {
                    *status = 128 + SIGTSTP;
                }
                return t->pid;
            }
        }
        if (!children) {
            return -ECHILD;
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
    strcpy(current->name, "idle");
    strcpy(current->cmd, "idle");
    idle = current;
}
