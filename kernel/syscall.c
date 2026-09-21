/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * syscall.c - the system call table and the wrappers around it.
 *
 * Everything above this file -- the shell today, user programs later --
 * reaches the kernel only through `trap #0`. Nothing calls the VFS or a
 * driver directly, which is what makes the boundary real rather than
 * decorative: when programs start running unprivileged, the code above
 * does not change at all, because it was never on this side of the gate
 * to begin with.
 *
 * Pointer arguments are still taken at face value. That is correct while
 * every caller shares the kernel's address space, and this is the
 * function that will have to validate and copy them when that stops
 * being true. It is marked here rather than in a plan somewhere because
 * this is where the work goes.
 */
#include "syscall.h"
#include "vfs.h"
#include "exec.h"
#include "job.h"
#include "tty.h"
#include "timer.h"
#include "dev.h"
#include "console.h"
#include "errno.h"
#include "string.h"

/* ---------------------------------------------------------------- */
/* The trap itself                                                   */
/* ---------------------------------------------------------------- */

s32 syscall0(u32 nr)
{
    register u32 d0 __asm__("d0") = nr;

    __asm__ volatile ("trap #0" : "+d"(d0) : : "memory", "cc");
    return (s32)d0;
}

s32 syscall1(u32 nr, u32 a1)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;

    __asm__ volatile ("trap #0" : "+d"(d0) : "d"(d1) : "memory", "cc");
    return (s32)d0;
}

s32 syscall2(u32 nr, u32 a1, u32 a2)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;
    register u32 d2 __asm__("d2") = a2;

    __asm__ volatile ("trap #0"
                      : "+d"(d0) : "d"(d1), "d"(d2) : "memory", "cc");
    return (s32)d0;
}

s32 syscall3(u32 nr, u32 a1, u32 a2, u32 a3)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;
    register u32 d2 __asm__("d2") = a2;
    register u32 d3 __asm__("d3") = a3;

    __asm__ volatile ("trap #0"
                      : "+d"(d0)
                      : "d"(d1), "d"(d2), "d"(d3) : "memory", "cc");
    return (s32)d0;
}

/* ---------------------------------------------------------------- */
/* The implementations                                               */
/* ---------------------------------------------------------------- */

static time_t clock_now(void)
{
    struct rtcdev *r = dev_rtc();
    time_t now = 0;

    if (r && r->get(r, &now) == 0) {
        return now;
    }
    return 0;
}

static int do_uname(struct utsname *u)
{
    if (!u) {
        return -EINVAL;
    }
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, KERNEL_NAME);
    strcpy(u->release, kernel_version);
    strcpy(u->machine, "m68040");
    strcpy(u->version, kernel_build);
    return 0;
}

static int do_stime(const time_t *t)
{
    struct rtcdev *r = dev_rtc();

    if (!t) {
        return -EINVAL;
    }
    if (!r || !r->set) {
        return -ENODEV;
    }
    return r->set(r, *t);
}

static void do_reboot(int cmd)
{
    vfs_sync();

    /*
     * RB_HALT_SYSTEM stops the CPU and leaves the machine sitting there,
     * which is what `halt` has always done here: the emulator keeps
     * running and a harness watching the serial output is what notices.
     *
     * The other two ask for the machine to actually go away, and a
     * driver may know how to make it. On this board one does -- the
     * keyboard controller has a reset line, which is how every PC since
     * 1984 has rebooted itself. If none does, saying so and halting is
     * better than pretending, because "shutdown" that silently did
     * nothing would be worse than one that says it cannot.
     */
    if ((u32)cmd != RB_HALT_SYSTEM) {
        if (dev_poweroff() == 0) {
            /* It worked; the machine is already going. */
            for (;;) {
                halt();
            }
        }
        kputln("reboot: nothing on this board can cut the power; halting");
    }
    halt();
}

/* --- jobs ---------------------------------------------------------- */

static int info_state(int state)
{
    switch (state) {
    case JOB_NEW:     return JOB_S_NEW;
    case JOB_RUNNING: return JOB_S_RUNNING;
    case JOB_STOPPED: return JOB_S_STOPPED;
    default:          return JOB_S_DONE;
    }
}

static int do_jobctl(int cmd, int arg, u32 p)
{
    struct job *j;

    switch (cmd) {
    case JOBCTL_INFO: {
        struct job_info *out = (struct job_info *)p;

        if (!out) {
            return -EINVAL;
        }
        j = job_nth(arg);
        if (!j) {
            return -ENOENT;
        }
        out->id = j->id;
        out->state = info_state(j->state);
        out->background = j->background;
        out->status = j->status;
        out->signalled = j->signalled;
        strncpy(out->cmd, j->cmd, sizeof(out->cmd) - 1);
        out->cmd[sizeof(out->cmd) - 1] = '\0';
        return 0;
    }

    case JOBCTL_FG:
        /*
         * Only a stopped job is resumed here. One that was queued with &
         * has never run, so there is no context to return into -- the
         * shell still has its command line and starts it the ordinary
         * way, which is the same thing with less machinery.
         */
        return exec_continue(arg);

    case JOBCTL_BG:
        j = job_get(arg);
        if (!j) {
            return -ENOENT;
        }
        /*
         * Running in the background means running while the shell also
         * runs, and nothing here can do two things at once yet. The job
         * keeps its place in the table and stays resumable with fg; what
         * is missing is a scheduler, and saying so is more use than
         * quietly running it in the foreground instead.
         */
        j->background = 1;
        return -ENOSYS;

    case JOBCTL_QUEUE:
        if (!p) {
            return -EINVAL;
        }
        return job_create((const char *)p, 1);

    case JOBCTL_REAP:
        job_reap();
        return 0;

    case JOBCTL_DROP:
        j = job_get(arg);
        if (!j) {
            return -ENOENT;
        }
        if (j->state == JOB_RUNNING || j->state == JOB_STOPPED) {
            return -EBUSY;
        }
        j->state = JOB_FREE;
        job_reap();
        return 0;

    default:
        return -EINVAL;
    }
}

/*
 * The table.
 *
 * Unimplemented numbers return -ENOSYS rather than doing something
 * approximate, because a call that silently does nothing is far harder
 * to find than one that says it does not exist.
 */
static s32 do_syscall(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    (void)a4;
    (void)a5;

    switch (nr) {
    case __NR_open:
        return fd_open((const char *)a1, (int)a2);

    case __NR_close:
        return fd_close((int)a1);

    case __NR_read:
        return fd_read((int)a1, (void *)a2, a3);

    case __NR_write:
        return fd_write((int)a1, (const void *)a2, a3);

    case __NR_lseek:
        return fd_lseek((int)a1, (s32)a2, (int)a3);

    case __NR_ioctl:
        return fd_ioctl((int)a1, a2, a3);

    case __NR_unlink:
        return vfs_unlink((const char *)a1);

    case __NR_rename:
        return vfs_rename((const char *)a1, (const char *)a2);

    case __NR_stat:
        return vfs_stat((const char *)a1, (struct stat *)a2);

    case __NR_getdents:
        return vfs_readdir((int)a1, (struct dirent *)a2);

    case __NR_statfs:
        return vfs_statfs((struct statfs *)a1);

    case __NR_fsync:
    case __NR_sync:
        return vfs_sync();

    case __NR_time: {
        time_t now = clock_now();

        if (a1) {
            *(time_t *)a1 = now;
        }
        return (s32)now;
    }

    case __NR_stime:
        return do_stime((const time_t *)a1);

    case __NR_uname:
        return do_uname((struct utsname *)a1);

    case __NR_reboot:
        do_reboot((int)a1);
        return 0;               /* not reached */

    case __NR_spawn:
        return exec_spawn((const char *)a1, (int)a2, (char **)a3);

    case __NR_jobctl:
        return do_jobctl((int)a1, (int)a2, a3);

    case __NR_times:
        return (s32)timer_jiffies();

    case __NR_nanosleep: {
        const struct timespec *req = (const struct timespec *)a1;
        u32 ms;

        if (!req) {
            return -EINVAL;
        }
        if (req->tv_nsec >= 1000000000UL) {
            return -EINVAL;
        }
        /* Milliseconds is as fine as a 10 ms tick can express, and
         * rounding to it before the tick calculation keeps the
         * arithmetic clear of overflow. */
        ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
        if (a2) {
            struct timespec *rem = (struct timespec *)a2;

            /* Nothing interrupts a sleep yet -- no signals, one program
             * -- so there is never any remaining. Zeroed rather than
             * left alone, because a caller checking it deserves a
             * defined answer. */
            rem->tv_sec = 0;
            rem->tv_nsec = 0;
        }
        return timer_sleep_ms(ms);
    }

    case __NR_exit:
        /* Returns only if nothing was spawned -- a program's exit()
         * unwinds all the way back into exec_spawn() and never comes
         * back here. */
        return exec_exit((int)a1);

    default:
        return -ENOSYS;
    }
}

/* ---------------------------------------------------------------- */
/* How deep in the kernel we are                                     */
/*                                                                    */
/* Counted because ctrl-C has to know. Killing a program means        */
/* throwing its whole stack away, and doing that from the middle of   */
/* a directory update leaves the directory half written. So a signal  */
/* raised while the kernel is working waits for the boundary, which   */
/* is a few instructions away, and one raised while the program is    */
/* running its own code is delivered by the timer tick straight away. */
/*                                                                    */
/* A count rather than a flag: the shell calls system calls too, and  */
/* spawn runs a whole program from inside one.                        */
/* ---------------------------------------------------------------- */

static volatile int depth;

int syscall_in_kernel(void)
{
    return depth != 0;
}

/*
 * A program that was killed or exited left the count wherever it was
 * when it jumped out, because nothing returned. exec.c calls this once
 * it is back on its own stack.
 */
void syscall_depth_reset(void)
{
    depth = 0;
}

s32 syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    s32 r;

    depth++;
    r = do_syscall(nr, a1, a2, a3, a4, a5);
    depth--;

    /*
     * The boundary. The kernel has finished whatever it was doing and is
     * about to hand control back, which makes this the one place a
     * program can be killed or stopped without leaving anything half
     * done. Note it happens on the way OUT: a read that was interrupted
     * has already returned EINTR, and the program never sees it.
     */
    if (depth == 0) {
        job_deliver(JOB_AT_SYSCALL);
    }
    return r;
}

/* ---------------------------------------------------------------- */
/* Wrappers                                                          */
/* ---------------------------------------------------------------- */

int sys_open(const char *path, int flags)
{
    return (int)syscall2(__NR_open, (u32)path, (u32)flags);
}

int sys_close(int fd)
{
    return (int)syscall1(__NR_close, (u32)fd);
}

s32 sys_read(int fd, void *buf, u32 len)
{
    return syscall3(__NR_read, (u32)fd, (u32)buf, len);
}

s32 sys_write(int fd, const void *buf, u32 len)
{
    return syscall3(__NR_write, (u32)fd, (u32)buf, len);
}

s32 sys_lseek(int fd, s32 offset, int whence)
{
    return syscall3(__NR_lseek, (u32)fd, (u32)offset, (u32)whence);
}

int sys_unlink(const char *path)
{
    return (int)syscall1(__NR_unlink, (u32)path);
}

int sys_rename(const char *from, const char *to)
{
    return (int)syscall2(__NR_rename, (u32)from, (u32)to);
}

int sys_stat(const char *path, void *st)
{
    return (int)syscall2(__NR_stat, (u32)path, (u32)st);
}

int sys_getdents(int index, void *dirent)
{
    return (int)syscall2(__NR_getdents, (u32)index, (u32)dirent);
}

int sys_statfs(void *sfs)
{
    return (int)syscall1(__NR_statfs, (u32)sfs);
}

int sys_fsync(int fd)
{
    return (int)syscall1(__NR_fsync, (u32)fd);
}

int sys_sync(void)
{
    return (int)syscall0(__NR_sync);
}

int sys_uname(struct utsname *u)
{
    return (int)syscall1(__NR_uname, (u32)u);
}

int sys_ioctl(int fd, u32 request, u32 arg)
{
    return (int)syscall3(__NR_ioctl, (u32)fd, request, arg);
}

int sys_spawn(const char *path, int argc, char **argv)
{
    return (int)syscall3(__NR_spawn, (u32)path, (u32)argc, (u32)argv);
}

int sys_jobctl(int cmd, int arg, void *p)
{
    return (int)syscall3(__NR_jobctl, (u32)cmd, (u32)arg, (u32)p);
}

u32 sys_times(void)
{
    return (u32)syscall0(__NR_times);
}

int sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
    return (int)syscall2(__NR_nanosleep, (u32)req, (u32)rem);
}

void sys_exit(int status)
{
    syscall1(__NR_exit, (u32)status);
}

time_t sys_time(time_t *t)
{
    return (time_t)syscall1(__NR_time, (u32)t);
}

int sys_stime(const time_t *t)
{
    return (int)syscall1(__NR_stime, (u32)t);
}

void sys_reboot(int cmd)
{
    syscall1(__NR_reboot, (u32)cmd);
    halt();                     /* the call does not return */
}
