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

#define __NR_exit        1
#define __NR_read        3
#define __NR_write       4
#define __NR_open        5
#define __NR_close       6
#define __NR_unlink     10
#define __NR_time       13
#define __NR_lseek      19
#define __NR_stime      25
#define __NR_rename     38
#define __NR_ioctl      54
#define __NR_reboot     88
#define __NR_statfs     99
#define __NR_stat      106
#define __NR_fsync     118
#define __NR_uname     122
#define __NR_getdents  141
#define __NR_sync      166      /* Linux has 36; 166 keeps it clear of
                                 * this table's own use of 36..38      */

/* Standard descriptors, bound to the console at startup. */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* What uname() fills in. */
struct utsname {
    char sysname[16];
    char release[16];
    char machine[16];
    char version[32];
};

/* The dispatcher, called from _trap0_entry in start.s. */
s32 syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5);

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
time_t sys_time(time_t *t);
int  sys_stime(const time_t *t);
void sys_reboot(int cmd) __attribute__((noreturn));

/* reboot() commands, Linux's magic values cut down to what is useful. */
#define RB_HALT_SYSTEM  0xcdef0123
#define RB_AUTOBOOT     0x01234567

#endif /* SYSCALL_H */
