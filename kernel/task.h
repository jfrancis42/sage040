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

#define TASK_MAX        64
#define TASK_NAME_MAX   24

/* How long a task runs before the tick offers the processor elsewhere.
 * Five ticks is 50 ms: long enough that switching costs nothing
 * measurable, short enough that a compute-bound program does not make
 * the machine feel stuck. */
#define TASK_SLICE      5       /* ticks a turn lasts at nice 0 */

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,                 /* wants the processor                 */
    TASK_RUNNING,               /* has it                              */
    TASK_BLOCKED,               /* waiting for something               */
    TASK_STOPPED,               /* ctrl-Z; will not run until continued */
    TASK_ZOMBIE                 /* finished, waiting to be reaped      */
};

struct task {
    /*
     * The address space this task's uaccess calls reach, when it is not
     * its own: exec, filling a new program's. PER TASK, because exec
     * sleeps now (the disk is interrupt-driven), and while it does other
     * tasks run -- a global override made every one of them read and
     * write the half-built program instead of itself. See uaccess.c.
     */
    struct addrspace *ua_override;

    int   pid;
    int   state;
    u32   ksp;                  /* the context: see the note above     */
    u32   kstack;               /* base of the block, guard page first */

    struct addrspace *as;       /* null for a kernel task              */

    /*
     * Its descriptors -- a table that may be SHARED. A process has one
     * of its own; the threads of one process all point at the same one,
     * because POSIX says a descriptor opened by any thread is a
     * descriptor every thread has. fork() takes a copy, clone() with
     * CLONE_FILES takes a reference. See fdtable in vfs.h.
     */
    struct fdtable *files;

    int   exit_status;
    int   signalled;            /* the signal that ended it, or 0      */
    struct task *parent;
    int   pgid;                 /* process group: what ctrl-C reaches  */
    int   sid;                  /* session: the groups a login holds   */

    /*
     * THREADS. A task is a thread; a process is every task sharing a
     * thread group id. For a process of one -- which is all there was
     * before, and still most of them -- tgid == pid and none of the
     * rest of this matters.
     *
     * getpid() reports the tgid and gettid() the pid, which is Linux's
     * split and the reason a threaded program's every thread agrees
     * about what process it is. A non-leader is never a child as far
     * as wait() is concerned: a thread is not something its parent
     * waits for, its joiner waits for it.
     */
    int   tgid;                 /* the process this thread belongs to  */

    /*
     * The word to zero and wake when this task ends, set by clone with
     * CLONE_CHILD_CLEARTID and by set_tid_address. This IS pthread_join:
     * the joiner sleeps on that address in a futex, and the dying
     * thread's last act in the kernel is to clear it and wake.
     */
    u32   clear_child_tid;

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
    int   nice;                 /* -20..19: how long its turns are     */
    u32   ss_sp, ss_size;       /* sigaltstack; size 0 when there is none */
    int   background;
    int   exiting;
    int   stop_reported;        /* its stop has been told to the parent */
    int   continued;            /* resumed, and not yet told (WCONTINUED) */

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
    u32   root_ino;             /* chroot: where "/" is, 0 the real one */
    char  cwd_path[PATH_MAX];
    u32   umask;                /* kept and reported; FAT has no modes  */

    /*
     * WHO THE TASK BELONGS TO.
     *
     * Real, effective and saved, as POSIX has them, and they are kept
     * properly rather than reported as a constant 0 -- which is what
     * this did until there were users at all.
     *
     * BE CLEAR WHAT THIS DOES AND DOES NOT BUY. The identity is real:
     * it is inherited across fork, kept across exec, changed by
     * setuid() under the usual rules, and reported by getuid() and
     * friends. What it cannot yet do is DECIDE ANYTHING ABOUT A FILE,
     * because a FAT volume has nowhere to record an owner or a mode --
     * so there is no such thing here as a file another user may not
     * read. Enforcement waits for a filesystem that can hold it
     * (task 36); until then this is identity without authority, which
     * is worth having by itself: it is what lets /etc/passwd mean
     * something, what `id` and `whoami` answer from, what a login
     * would set, and what ssh will authenticate into.
     *
     * Saying so plainly matters more than the feature does. A system
     * that reports users and enforces nothing is a system somebody
     * could mistake for one that enforces something.
     */
    u32   uid, euid, suid;      /* real, effective, saved-set          */
    u32   gid, egid, sgid;

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
    /* The user and system ticks of the child the last wait reaped,
     * its own children's included -- what wait4 reports as its rusage. */
    u32   waited_utime, waited_stime;

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

/* fork(): a copy of the current task. Null if there is not the memory. */
struct pt_regs;
struct task *task_fork(struct pt_regs *regs);

/*
 * clone(): fork's general form, and how a thread is made.
 *
 * With CLONE_VM|CLONE_THREAD (and the rest of the thread set) the new
 * task shares this one's address space, descriptors, working directory
 * and signal handlers, starts on the stack it is given, and belongs to
 * the same thread group. Returns the new task, or null with *err set.
 */
struct task *task_clone(struct pt_regs *regs, u32 flags, u32 child_stack,
                        u32 ptid, u32 ctid, u32 tls, int *err);

/* How many tasks share this one's thread group, itself included. */
int  task_group_count(struct task *t);

/* End every OTHER thread of this task's process, and wait for them to
 * be gone. exit_group() and execve() both need it. */
void task_group_kill(struct task *t);

/* Made by exec.c, which builds the address space and user stack first. */
struct task *task_create_user(const char *name, u32 entry, u32 usp,
                              struct addrspace *as);

/* Wake anything whose sleep deadline has passed. From the tick. */
void task_timeouts(void);

/* The one running now. Never null once task_init() has run: the idle
 * task is a task. */
extern struct task *current;

/*
 * May the running code sleep? Not in the idle task -- it is what runs
 * when nothing else can, so there would be nothing to switch to and
 * nothing to come back -- and not in an interrupt handler.
 */
int task_can_sleep(void);

/* The kernel stack's size, the most of it any task has used, and which
 * task that was. See task.c. */
void task_kstack_stats(u32 *size, u32 *max_used, char *name, u32 namelen);

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
int task_slice(const struct task *t);   /* ticks in a turn, from nice */
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
void task_cred_inherit(struct task *t, struct task *from);

/* Never returns. */
void task_exit(int status) __attribute__((noreturn));

/*
 * Wait for a child to change state, waitpid()'s way: `pid` > 0 is that
 * child, -1 any, 0 any in the caller's group, -N any in group N. The
 * status is Linux's encoding. Returns the child's pid, 0 with WNOHANG
 * and nothing to report, -ECHILD, or -EINTR for a signal.
 */
int  task_wait(int pid, int *status, int options);

/* Let go of a zombie's last resources. */
void task_reap(struct task *t);

int  task_count(void);
const char *task_state_name(int state);

#endif /* TASK_H */
