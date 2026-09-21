/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * syscall.h - the system call interface.
 *
 * The calling convention is Linux/m68k's, exactly:
 *
 *   d0 = call number
 *   d1, d2, d3, d4, d5 = arguments
 *   trap #0
 *   d0 = result, or a negated errno
 *
 * That is not an imitation of Linux, it is the same convention, because
 * Linux on m68k picked the obvious one for this architecture and there
 * is nothing to improve on. The numbers are Linux's i386 numbers, which
 * is the set most people recognise -- __NR_write being 4 is a fact a lot
 * of people carry around.
 *
 * Calls return a non-negative result or a negated error. There is no
 * global errno; see errno.h for why.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include "kernel.h"
#include "uapi.h"

/* The dispatcher, called from _trap0_entry in start.s. */
s32 syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5);

/*
 * Is the kernel inside a system call right now?
 *
 * Asked by the timer tick before it unwinds a program for ctrl-C: the
 * answer decides whether throwing the stack away is safe or would leave
 * a directory half written.
 */
int  syscall_in_kernel(void);

/*
 * Install a new system-call depth and return the old one.
 *
 * Used when control crosses between the kernel's own work and a
 * program's: the count is per-program, because a program entered from
 * inside the shell's spawn would otherwise never appear to reach a
 * system call boundary at all.
 */
int  syscall_depth_swap(int d);
int  syscall_depth(void);

/*
 * Make a system call the way a user program will -- through the trap,
 * not around it. The kernel's own code uses these so that the gate is
 * exercised by everything, rather than being a path only future user
 * programs take and nobody has tried.
 */
s32 syscall0(u32 nr);
s32 syscall1(u32 nr, u32 a1);
s32 syscall2(u32 nr, u32 a1, u32 a2);
s32 syscall3(u32 nr, u32 a1, u32 a2, u32 a3);

/* Thin wrappers, so callers read like C rather than like assembly. */
int  sys_open(const char *path, int flags);
int  sys_close(int fd);
s32  sys_read(int fd, void *buf, u32 len);
s32  sys_write(int fd, const void *buf, u32 len);
s32  sys_lseek(int fd, s32 offset, int whence);
int  sys_unlink(const char *path);
int  sys_rename(const char *from, const char *to);
int  sys_stat(const char *path, void *st);
int  sys_getdents(int index, void *dirent);
int  sys_statfs(void *sfs);
int  sys_fsync(int fd);
int  sys_sync(void);
int  sys_uname(struct utsname *u);
int  sys_sysinfo(struct sysinfo *si);
int  sys_ioctl(int fd, u32 request, u32 arg);
int  sys_spawn(const char *path, int argc, char **argv);
int  sys_jobctl(int cmd, int arg, void *p);
int  sys_netctl(int cmd, u32 arg, void *p);
int  sys_socket(int domain, int type, int protocol);
int  sys_connect(int fd, const struct sockaddr_in *addr);
u32  sys_times(void);
int  sys_nanosleep(const struct timespec *req, struct timespec *rem);
void sys_exit(int status);
time_t sys_time(time_t *t);
int  sys_stime(const time_t *t);
void sys_reboot(int cmd) __attribute__((noreturn));

#endif /* SYSCALL_H */
