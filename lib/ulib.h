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

/* Error numbers are part of the ABI: every call here returns one,
 * negated, on failure. Pure macros, and Linux's values. */
#include "errno.h"

/* System calls. Same trap, same numbers -- there is no other way in. */
int    open(const char *path, int flags);
int    close(int fd);
s32    read(int fd, void *buf, u32 len);
s32    write(int fd, const void *buf, u32 len);
s32    lseek(int fd, s32 offset, int whence);
int    ioctl(int fd, u32 request, u32 arg);
int    unlink(const char *path);
int    stat(const char *path, struct stat *st);
int    fstat(int fd, struct stat *st);
int    access(const char *path, int mode);
int    dup(int fd);
int    dup2(int oldfd, int newfd);

/*
 * The heap. brk() sets the break and returns 0, or -ENOMEM with the
 * break unchanged; sbrk() moves it by `incr` and returns where it WAS,
 * or (void *)-1. sbrk(0) says where it is. The heap starts on the page
 * after the program image and may grow to a page below the stack.
 */
int    brk(void *addr);

/* Memory and uptime, in pages of mem_unit bytes. */
int    sysinfo(struct sysinfo *si);
void  *sbrk(s32 incr);

/* Is this descriptor a terminal? Built on fstat, the way it is
 * everywhere: there is no separate call to ask. */
int    isatty(int fd);
int    getdents(int index, struct dirent *d);

/* Directories. The working directory is per task, so a chdir() here
 * moves this program and nothing else. */
int    chdir(const char *path);
int    getcwd(char *buf, u32 size);
int    mkdir(const char *path);
int    rmdir(const char *path);
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

/*
 * Sockets. A socket is a file descriptor, so read(), write() and
 * close() work on one -- there is no send()/recv() pair for a stream.
 *
 * htons() and friends are the identity on this machine, because network
 * byte order IS big-endian and so is a 68040. Call them anyway: the
 * habit is what makes the code portable and it costs nothing here.
 */
int    socket(int domain, int type, int protocol);
int    bind(int fd, const struct sockaddr_in *addr);
int    connect(int fd, const struct sockaddr_in *addr);
int    listen(int fd, int backlog);
int    accept(int fd, struct sockaddr_in *addr);
s32    sendto(int fd, const void *buf, u32 len, const struct sockaddr_in *to);
s32    recvfrom(int fd, void *buf, u32 len, struct sockaddr_in *from);
int    shutdown(int fd, int how);

/*
 * The network control call. Local to this system -- Linux does this
 * with ioctls on a socket, which needs a socket for something that is
 * not a connection. See uapi.h for the commands.
 */
int    netctl(int cmd, u32 arg, void *p);

/* Formatting helpers that every network tool needs. */
void   put_ip(u32 addr);
void   put_mac(const u8 *mac);

#define htons(x) ((u16)(x))
#define ntohs(x) ((u16)(x))
#define htonl(x) ((u32)(x))
#define ntohl(x) ((u32)(x))

/* "10.1.0.1" -> an address, or 0. */
u32    inet_aton(const char *s);

/* Output helpers, all of them eventually write(). */
void   putch(char c);
void   puts(const char *s);          /* no newline appended */
void   putdec(u32 v);
void   puthex(u32 v);
void   eputs(const char *s);         /* to stderr */

/* Has a key been pressed?  Does not block and does not consume it. */
int    key_waiting(void);

/* The environment this program was started with. Read only. */
const char *getenv(const char *name);
extern char **environ;

u32    strlen(const char *s);
int    strcmp(const char *a, const char *b);
void  *memset(void *dst, int c, u32 n);
void  *memcpy(void *dst, const void *src, u32 n);

#endif /* ULIB_H */
