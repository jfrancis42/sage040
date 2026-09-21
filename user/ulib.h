/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ulib.h - the C library, such as it is.
 *
 * A program gets two headers: this one and the kernel's uapi.h, which is
 * the system call ABI and nothing else. It does not get kernel.h, vfs.h,
 * dev.h or anything under drivers/ -- those describe the inside of the
 * kernel and a program has no business seeing them.
 *
 * There are no exceptions. A program that draws opens /dev/fb0 and uses
 * the FBIO_* ioctls like any other device; the hardware header is not on
 * the include path at all, so reaching a chip would mean editing
 * user/Makefile rather than adding an #include.
 */
#ifndef ULIB_H
#define ULIB_H

#include "uapi.h"

/* System calls. Same trap, same numbers -- there is no other way in. */
int    open(const char *path, int flags);
int    close(int fd);
s32    read(int fd, void *buf, u32 len);
s32    write(int fd, const void *buf, u32 len);
s32    lseek(int fd, s32 offset, int whence);
int    ioctl(int fd, u32 request, u32 arg);
int    unlink(const char *path);
int    stat(const char *path, struct stat *st);
int    getdents(int index, struct dirent *d);
int    uname(struct utsname *u);
time_t time(time_t *t);
int    fsync(int fd);
u32    times(void);                  /* ticks since boot */
int    nanosleep(const struct timespec *req, struct timespec *rem);
void   msleep(u32 ms);
void   exit(int status) __attribute__((noreturn));
/*
 * Stop the machine. RB_POWER_OFF asks the board to actually go away and
 * RB_HALT_SYSTEM just stops the processor; whether either is possible is
 * the kernel's business, not a program's. Does not return if it works.
 */
void   reboot(int cmd);

/* Output helpers, all of them eventually write(). */
void   putch(char c);
void   puts(const char *s);          /* no newline appended */
void   putdec(u32 v);
void   puthex(u32 v);
void   eputs(const char *s);         /* to stderr */

/* Has a key been pressed?  Does not block and does not consume it. */
int    key_waiting(void);

u32    strlen(const char *s);
int    strcmp(const char *a, const char *b);
void  *memset(void *dst, int c, u32 n);
void  *memcpy(void *dst, const void *src, u32 n);

#endif /* ULIB_H */
