/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sysuser.c - the calling side of the system call gate.
 *
 * The trap and one wrapper per call. Everything here is code a PROGRAM
 * could run, because that is what it is used as: the kernel's own shell
 * calls these from a kernel task, and /bin/sh -- the same shell.c,
 * built as a program -- calls exactly the same functions from user
 * mode. They were at the bottom of syscall.c, next to the dispatcher,
 * until the shell needed them without the kernel around them.
 */
#include "syscall.h"

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

s32 syscall4(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;
    register u32 d2 __asm__("d2") = a2;
    register u32 d3 __asm__("d3") = a3;
    register u32 d4 __asm__("d4") = a4;

    __asm__ volatile ("trap #0"
                      : "+d"(d0)
                      : "d"(d1), "d"(d2), "d"(d3), "d"(d4)
                      : "memory", "cc");
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

/*
 * The mounted volume's name. Not part of statfs, because statfs is
 * Linux's structure and Linux's has no field for it (uapi.h,
 * FSCTL_LABEL).
 */
int sys_fslabel(void *label)
{
    return (int)syscall3(__NR_fsctl, FSCTL_LABEL, 0, (u32)label);
}

/* Who this task belongs to. See struct task for what that does buy. */
int sys_getuid(void)
{
    return (int)syscall0(__NR_getuid);
}

int sys_getgid(void)
{
    return (int)syscall0(__NR_getgid);
}

int sys_fsync(int fd)
{
    return (int)syscall1(__NR_fsync, (u32)fd);
}

int sys_sync(void)
{
    return (int)syscall0(__NR_sync);
}

int sys_sysinfo(struct sysinfo *si)
{
    return (int)syscall1(__NR_sysinfo, (u32)si);
}

int sys_uname(struct utsname *u)
{
    return (int)syscall1(__NR_uname, (u32)u);
}

int sys_ioctl(int fd, u32 request, u32 arg)
{
    return (int)syscall3(__NR_ioctl, (u32)fd, request, arg);
}

int sys_spawn(const char *path, int argc, char **argv, char **envp)
{
    return (int)syscall4(__NR_spawn, (u32)path, (u32)argc, (u32)argv,
                         (u32)envp);
}

int sys_jobctl(int cmd, int arg, void *p)
{
    return (int)syscall3(__NR_jobctl, (u32)cmd, (u32)arg, (u32)p);
}

int sys_mkdir(const char *path)
{
    return (int)syscall1(__NR_mkdir, (u32)path);
}

int sys_rmdir(const char *path)
{
    return (int)syscall1(__NR_rmdir, (u32)path);
}

int sys_chdir(const char *path)
{
    return (int)syscall1(__NR_chdir, (u32)path);
}

int sys_getcwd(char *buf, u32 len)
{
    return (int)syscall2(__NR_getcwd, (u32)buf, len);
}

int sys_waitpid(int pid, int *status, int options)
{
    return (int)syscall3(__NR_waitpid, (u32)pid, (u32)status, (u32)options);
}

int sys_kill(int pid, int sig)
{
    return (int)syscall2(__NR_kill, (u32)pid, (u32)sig);
}

int sys_getpid(void)
{
    return (int)syscall0(__NR_getpid);
}

int sys_setpgid(int pid, int pgid)
{
    return (int)syscall2(__NR_setpgid, (u32)pid, (u32)pgid);
}

int sys_pipe(int fds[2])
{
    return (int)syscall1(__NR_pipe, (u32)fds);
}

int sys_dup(int fd)
{
    return (int)syscall1(__NR_dup, (u32)fd);
}

int sys_dup2(int oldfd, int newfd)
{
    return (int)syscall2(__NR_dup2, (u32)oldfd, (u32)newfd);
}

int sys_fcntl(int fd, int cmd, u32 arg)
{
    return (int)syscall3(__NR_fcntl, (u32)fd, (u32)cmd, arg);
}

void sys_yield(void)
{
    syscall0(__NR_sched_yield);
}

int sys_netctl(int cmd, u32 arg, void *p)
{
    return (int)syscall3(__NR_netctl, (u32)cmd, arg, (u32)p);
}

int sys_socket(int domain, int type, int protocol)
{
    return (int)syscall3(__NR_socket, (u32)domain, (u32)type, (u32)protocol);
}

int sys_connect(int fd, const struct sockaddr_in *addr)
{
    return (int)syscall2(__NR_connect, (u32)fd, (u32)addr);
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
    for (;;) {
        /* the call does not return */
    }
}
