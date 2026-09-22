/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * task.h - something that runs.
 *
 * A task is what `struct job` was growing into. The job table already
 * carried an identity, a state, an address space, a kernel stack and a
 * saved context; what it lacked was a notion of runnable-versus-blocked
 * and anything to choose between two of them. This adds both, and the
 * shell's jobs become a view onto it rather than a table of their own.
 *
 * THE CONTEXT IS THE KERNEL STACK POINTER, and nothing else.
 *
 * That is the whole trick, and it is worth stating before any of the
 * rest makes sense. A task that is not running is sitting inside
 * schedule(), which was called either voluntarily -- it blocked -- or
 * from the tail of an interrupt. Either way its registers are already on
 * its own kernel stack. So "saving the context" is remembering one
 * pointer, and "restoring" it is putting that pointer back in A7 and
 * returning; the ordinary function epilogue and the ordinary RTE do the
 * rest.
 *
 * It follows that a BRAND NEW task needs a kernel stack that has been
 * made to look as though it had been suspended that way. task_new()
 * builds one: an exception frame claiming to come from user mode, a set
 * of zeroed registers under it, and a return address pointing at the
 * stub that pops them. The first time it is scheduled it returns into
 * that stub and RTEs into its entry point, having never run before.
 *
 * WHERE A SWITCH CAN HAPPEN
 *
 * Only on the way back to user mode. The timer marks the running task as
 * having had its turn, and the check happens at the end of an interrupt
 * or a system call, once the kernel has finished what it was doing. A
 * kernel that could be preempted anywhere would need every data
 * structure it owns to be prepared for it; a kernel preempted only at
 * the boundary needs none of that, and gives up only the ability to
 * schedule during a long system call.
 *
 * A task may of course give up the processor at any time by blocking,
 * and that is a different path -- sleep_on() rather than preemption --
 * but it is the same switch underneath.
 */
#ifndef TASK_H
#define TASK_H

#include "kernel.h"
#include "uapi.h"
#include "wait.h"

struct addrspace;
struct file;

#define TASK_MAX        8
#define TASK_NAME_MAX   24

/* How long a task runs before the tick offers the processor elsewhere.
 * Five ticks is 50 ms: long enough that switching costs nothing
 * measurable, short enough that a compute-bound program does not make
 * the machine feel stuck. */
#define TASK_SLICE      5

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,                 /* wants the processor                 */
    TASK_RUNNING,               /* has it                              */
    TASK_BLOCKED,               /* waiting for something               */
    TASK_STOPPED,               /* ctrl-Z; will not run until continued */
    TASK_ZOMBIE                 /* finished, waiting to be reaped      */
};

struct task {
    int   pid;
    int   state;
    u32   ksp;                  /* the context: see the note above     */
    u32   kstack;               /* base of the block, guard page first */

    struct addrspace *as;       /* null for a kernel task              */
    struct file *fds[OPEN_MAX]; /* its own descriptors                 */

    int   exit_status;
    int   signalled;            /* the signal that ended it, or 0      */
    struct task *parent;

    /*
     * Signals. A bitmask each, signal N in bit N-1 (SIGMASK), because
     * there are fewer than 32 -- and that is also the user's layout, so
     * a mask crosses the system call boundary unconverted.
     */
    volatile u32 sig_pending;
    u32   sig_blocked;
    struct sigaction sigact[NSIG];  /* [0] unused; SIG_DFL when zero   */

    /* sigsuspend() swaps the mask for the length of the wait, and the
     * old one comes back when the handler returns -- see signal.c. */
    u32   sig_saved_mask;
    int   sig_restore_mask;

    /*
     * The system call this task is in, while it is in one, or -1.
     * Signal delivery needs it: a call interrupted by a signal is
     * restarted by putting its number back in d0 and stepping back
     * over the trap, and that is only right for a call.
     */
    int   syscall_nr;

    int   slice;                /* ticks left in this turn             */
    int   background;
    int   exiting;
    int   stop_reported;        /* its stop has been told to the parent */

    /*
     * The working directory, as two halves: what the filesystem uses to
     * find it, and a printable form for pwd. It is per task because
     * that is what it means -- a program that does chdir() must not
     * move its parent, or its siblings, or the shell.
     *
     * The cluster number is stored as a plain u32 rather than a
     * filesystem type, so that nothing above the filesystem has to know
     * what a directory IS. Any other filesystem would put an inode
     * number here and be equally well served.
     */
    u32   cwd_ino;
    char  cwd_path[PATH_MAX];

    char  name[TASK_NAME_MAX];
    char  cmd[JOB_CMD_MAX];     /* the command line, for `jobs`        */

    struct task *wait_next;     /* linkage while on a wait queue       */
    struct waitq *queue;        /* which queue, or null                */
    int   woken;                /* somebody woke it, as opposed to a
                                 * timeout expiring                    */
    u32   wake_at;              /* jiffies, for a sleep with a deadline */

    /* Whoever is in task_wait() for one of this task's children. */
    struct waitq child_wait;

    /*
     * The FPU, while this task is not running. The 68040 has one set of
     * FP registers and one rounding mode, so without this every task
     * that used floating point would be sharing them with every other.
     *
     *     0   the fsave frame: 4 bytes idle, up to 96 busy
     *    96   fp0-fp7, 12 bytes each
     *   192   fpcr, fpsr, fpiar
     *
     * See taskasm.s. A new task starts with an IDLE frame and zeroed
     * registers, rather than a null frame, because under QEMU restoring
     * a null frame resets nothing and the new task would inherit the
     * previous one's registers.
     */
    u32   fpu[52];

    /*
     * Time, in ticks. A tick is charged to user or system time by what
     * the timer interrupt interrupted. The children's figures collect
     * those of children that have been waited for, as POSIX says.
     */
    u32   utime, stime;
    u32   cutime, cstime;

    /*
     * Interval timers. The real one is a deadline in jiffies, because it
     * runs whether or not the task does; the other two count down only
     * while the task is charged time. 0 means disarmed. The intervals
     * are the reload values, 0 for a one-shot.
     */
    u32   it_real_at, it_real_interval;
    u32   it_virt, it_virt_interval;
    u32   it_prof, it_prof_interval;
};

struct addrspace;

/* Made by exec.c, which builds the address space and user stack first. */
struct task *task_create_user(const char *name, u32 entry, u32 usp,
                              struct addrspace *as);

/* Wake anything whose sleep deadline has passed. From the tick. */
void task_timeouts(void);

/* The one running now. Never null once task_init() has run: the idle
 * task is a task. */
extern struct task *current;

void task_init(void);

/*
 * Make a task that will begin at `entry` in supervisor mode.
 *
 * Used for the kernel's own tasks -- the idle loop and the shell. A user
 * program is made by exec.c, which builds an address space first and
 * then hands it here.
 */
struct task *task_create(const char *name, void (*entry)(void));

struct task *task_find(int pid);
struct task *task_nth(int index);

/* Give up the processor. Returns when this task runs again. */
void schedule(void);

/* Mark the current task as having had its turn. Called from the tick. */
void task_tick(void);

/*
 * Called at the end of an interrupt or a system call, with the saved SR
 * of the interrupted context. Delivers signals and switches if the
 * current task's turn is over -- and does neither if the return is into
 * the kernel rather than into a program.
 */
struct pt_regs;
void task_ret_to_user(struct pt_regs *regs);

/* Give `t` the working directory `from` is standing in. */
void task_cwd_inherit(struct task *t, struct task *from);

/* Never returns. */
void task_exit(int status) __attribute__((noreturn));

/* Wait for a child to finish. Returns its pid, or -ECHILD. */
int  task_wait(int pid, int *status);

/* Let go of a zombie's last resources. */
void task_reap(struct task *t);

int  task_count(void);
const char *task_state_name(int state);

#endif /* TASK_H */
