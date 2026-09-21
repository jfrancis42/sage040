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
#include "dev.h"
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
    /*
     * There is no reset line to pull, so both commands end the same way:
     * flush anything the filesystem is holding, then stop the CPU. QEMU
     * keeps running; a harness watching the serial output is what
     * notices.
     */
    vfs_sync();
    (void)cmd;
    halt();
}

/*
 * The table.
 *
 * Unimplemented numbers return -ENOSYS rather than doing something
 * approximate, because a call that silently does nothing is far harder
 * to find than one that says it does not exist.
 */
s32 syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
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
