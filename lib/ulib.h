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

/* malloc, free, calloc, realloc: where <stdlib.h> would put them. */
#include "malloc.h"

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

/* Pipes and descriptor flags, with Linux's meanings. fcntl takes
 * F_DUPFD, F_GETFD/F_SETFD (FD_CLOEXEC) and F_GETFL/F_SETFL, where only
 * O_NONBLOCK and O_APPEND can change. */
int    pipe(int fds[2]);
int    fcntl(int fd, int cmd, u32 arg);

/*
 * The heap. brk() sets the break and returns 0, or -ENOMEM with the
 * break unchanged; sbrk() moves it by `incr` and returns where it WAS,
 * or (void *)-1. sbrk(0) says where it is. The heap starts on the page
 * after the program image and may grow to a page below the stack.
 */
int    brk(void *addr);

/*
 * Mapping memory, with Linux's meaning. mmap() returns MAP_FAILED on
 * any error; it has no errno to say which, so a program that needs to
 * know calls syscall(__NR_mmap2, ...) and reads the negated errno. See
 * uapi.h for what the flags do and do not do here.
 */
void  *mmap(void *addr, u32 len, int prot, int flags, int fd, u32 offset);
int    munmap(void *addr, u32 len);
int    mprotect(void *addr, u32 len, int prot);

/*
 * Processes and signals, with Linux's meanings. sigaction() always
 * installs the library's return trampoline; signal() is sigaction with
 * SA_RESTART, as glibc's is. A handler is an ordinary function of one
 * argument, the signal number. spawn() starts a program and returns its
 * pid without waiting; it is not fork().
 */
int    getpid(void);
int    kill(int pid, int sig);     /* 0: my group; -N: group N; -1: all */
int    getppid(void);
int    getpgrp(void);
int    setpgid(int pid, int pgid);
int    tcgetpgrp(int fd);
int    tcsetpgrp(int fd, int pgrp);
int    raise(int sig);
int    waitpid(int pid, int *status, int options);
int    spawn(const char *path, int argc, char **argv, char **envp);
int    fork(void);
int    execve(const char *path, char *const argv[], char *const envp[]);
int    execv(const char *path, char *const argv[]);
int    execvp(const char *file, char *const argv[]);
int    sigaction(int sig, const struct sigaction *act, struct sigaction *old);
sighandler_t signal(int sig, sighandler_t handler);
int    sigprocmask(int how, const sigset_t *set, sigset_t *old);
int    sigpending(sigset_t *set);
int    sigsuspend(const sigset_t *mask);
int    pause(void);
int    sigemptyset(sigset_t *s);
int    sigfillset(sigset_t *s);
int    sigaddset(sigset_t *s, int sig);
int    sigdelset(sigset_t *s, int sig);
int    sigismember(const sigset_t *s, int sig);

/*
 * Waiting on several descriptors. Linux's meanings; select() writes the
 * unused time back into *tv. See uapi.h for what readable means on a
 * terminal in canonical mode.
 */
int    poll(struct pollfd *fds, u32 n, int timeout_ms);
int    select(int nfds, fd_set *in, fd_set *out, fd_set *ex,
              struct timeval *tv);

/* Any system call by number: the result, or a negated errno. */
s32    syscall(u32 nr, ...);

/* Memory and uptime, in pages of mem_unit bytes. */
int    sysinfo(struct sysinfo *si);
void  *sbrk(s32 incr);

/* Is this descriptor a terminal? Built on fstat, the way it is
 * everywhere: there is no separate call to ask. */
int    isatty(int fd);
/* POSIX's names for TCGETS and TCSETS; the actions are Linux's values. */
#define TCSANOW    0
#define TCSADRAIN  1
#define TCSAFLUSH  2
int    tcgetattr(int fd, struct termios *t);
int    tcsetattr(int fd, int action, const struct termios *t);
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
u32    times(struct tms *buf);       /* ticks since boot; fills *buf if given */
int    gettimeofday(struct timeval *tv, void *tz);
int    settimeofday(const struct timeval *tv, const void *tz);
u32    alarm(u32 seconds);
int    setitimer(int which, const struct itimerval *in, struct itimerval *old);
int    getitimer(int which, struct itimerval *cur);
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
int    socketpair(int domain, int type, int protocol, int sv[2]);
int    bind(int fd, const struct sockaddr *addr, socklen_t len);
int    connect(int fd, const struct sockaddr *addr, socklen_t len);
int    listen(int fd, int backlog);
int    accept(int fd, struct sockaddr *addr, socklen_t *len);
int    accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
s32    send(int fd, const void *buf, u32 len, int flags);
s32    recv(int fd, void *buf, u32 len, int flags);
s32    sendto(int fd, const void *buf, u32 len, int flags,
              const struct sockaddr *to, socklen_t tolen);
s32    recvfrom(int fd, void *buf, u32 len, int flags,
                struct sockaddr *from, socklen_t *fromlen);
s32    sendmsg(int fd, const struct msghdr *msg, int flags);
s32    recvmsg(int fd, struct msghdr *msg, int flags);
int    getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int    getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int    setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int    getsockopt(int fd, int level, int name, void *val, socklen_t *len);
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

/* Dotted-quad addresses, in POSIX shape. */
u32    inet_addr(const char *s);         /* or INADDR_NONE */
int    inet_aton(const char *s, struct in_addr *out);
char  *inet_ntoa(struct in_addr in);

/*
 * A name to an address: a dotted quad, /etc/hosts, "localhost", then DNS
 * to the servers in /etc/resolv.conf or the one DHCP gave (resolv.c).
 * 0, or -ENOENT (no such name), -ETIMEDOUT (no answer), -ENETUNREACH
 * (nobody to ask).
 */
int    resolve_host(const char *name, struct in_addr *out);

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
int    memcmp(const void *a, const void *b, u32 n);

#endif /* ULIB_H */
