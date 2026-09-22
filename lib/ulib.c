/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ulib.c - system call stubs and the handful of helpers around them.
 *
 * The stubs are the same four lines of assembly the kernel uses on its
 * own behalf, and deliberately so: there is one way into the kernel and
 * everything takes it. d0 holds the call number, d1 to d5 the arguments,
 * d0 comes back with the result or a negated errno.
 *
 * There is no zero-argument form here because nothing needs one yet, and
 * an unused static is a build error in this tree.
 */
#include "ulib.h"

static s32 sc1(u32 nr, u32 a1)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;

    __asm__ volatile ("trap #0" : "+d"(d0) : "d"(d1) : "memory", "cc");
    return (s32)d0;
}

static s32 sc2(u32 nr, u32 a1, u32 a2)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;
    register u32 d2 __asm__("d2") = a2;

    __asm__ volatile ("trap #0"
                      : "+d"(d0) : "d"(d1), "d"(d2) : "memory", "cc");
    return (s32)d0;
}

static s32 sc3(u32 nr, u32 a1, u32 a2, u32 a3)
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

void reboot(int cmd)
{
    sc1(__NR_reboot, (u32)cmd);
}

static s32 sc4(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4)
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

/* Six arguments: d1-d5 and a0, as Linux/m68k passes them. Only mmap2
 * and syscall() need it. */
static s32 sc6(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;
    register u32 d2 __asm__("d2") = a2;
    register u32 d3 __asm__("d3") = a3;
    register u32 d4 __asm__("d4") = a4;
    register u32 d5 __asm__("d5") = a5;
    register u32 a0 __asm__("a0") = a6;

    __asm__ volatile ("trap #0"
                      : "+d"(d0)
                      : "d"(d1), "d"(d2), "d"(d3), "d"(d4), "d"(d5), "a"(a0)
                      : "memory", "cc");
    return (s32)d0;
}

/*
 * Any call by number, the way Linux's libc offers it. Always passes six
 * arguments; a call that takes fewer ignores the rest.
 */
s32 syscall(u32 nr, ...)
{
    __builtin_va_list ap;
    u32 a[6];
    int i;

    __builtin_va_start(ap, nr);
    for (i = 0; i < 6; i++) {
        a[i] = __builtin_va_arg(ap, u32);
    }
    __builtin_va_end(ap);
    return sc6(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
}

void *mmap(void *addr, u32 len, int prot, int flags, int fd, u32 offset)
{
    s32 r;

    /* mmap2 takes the offset in pages; one that is not whole pages is
     * refused here, as the kernel would refuse it through mmap. */
    if (offset & 4095) {
        return MAP_FAILED;
    }
    r = sc6(__NR_mmap2, (u32)addr, len, (u32)prot, (u32)flags, (u32)fd,
            offset / 4096);
    return r < 0 ? MAP_FAILED : (void *)r;
}

int poll(struct pollfd *fds, u32 n, int timeout_ms)
{
    return (int)sc3(__NR_poll, (u32)fds, n, (u32)timeout_ms);
}

int select(int nfds, fd_set *in, fd_set *out, fd_set *ex, struct timeval *tv)
{
    return (int)sc6(__NR__newselect, (u32)nfds, (u32)in, (u32)out, (u32)ex,
                    (u32)tv, 0);
}

int munmap(void *addr, u32 len)
{
    return (int)sc2(__NR_munmap, (u32)addr, len);
}

int mprotect(void *addr, u32 len, int prot)
{
    return (int)sc3(__NR_mprotect, (u32)addr, len, (u32)prot);
}

/* --- processes and signals ------------------------------------------ */

int getpid(void)
{
    return (int)sc1(__NR_getpid, 0);
}

int kill(int pid, int sig)
{
    return (int)sc2(__NR_kill, (u32)pid, (u32)sig);
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

int waitpid(int pid, int *status, int options)
{
    (void)options;              /* no WNOHANG yet */
    return (int)sc2(__NR_waitpid, (u32)pid, (u32)status);
}

int spawn(const char *path, int argc, char **argv, char **envp)
{
    return (int)sc4(__NR_spawn, (u32)path, (u32)argc, (u32)argv, (u32)envp);
}

extern void __sigreturn_trampoline(void);

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct sigaction k;

    if (!act) {
        return (int)sc3(__NR_sigaction, (u32)sig, 0, (u32)old);
    }
    /* Every handler returns through the library's trampoline. */
    k = *act;
    k.sa_flags |= SA_RESTORER;
    k.sa_restorer = __sigreturn_trampoline;
    return (int)sc3(__NR_sigaction, (u32)sig, (u32)&k, (u32)old);
}

/*
 * The BSD and glibc meaning: the handler stays installed, and a system
 * call it interrupts is restarted. System V's one-shot signal() is what
 * sigaction with SA_RESETHAND is for.
 */
sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act, old;

    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = SA_RESTART;
    act.sa_restorer = 0;
    if (sigaction(sig, &act, &old) < 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return (int)sc3(__NR_sigprocmask, (u32)how, (u32)set, (u32)old);
}

int sigpending(sigset_t *set)
{
    return (int)sc1(__NR_sigpending, (u32)set);
}

int sigsuspend(const sigset_t *mask)
{
    return (int)sc1(__NR_sigsuspend, *mask);
}

int pause(void)
{
    return (int)sc1(__NR_pause, 0);
}

int sigemptyset(sigset_t *s)          { *s = 0; return 0; }
int sigfillset(sigset_t *s)           { *s = 0xffffffffUL; return 0; }

int sigaddset(sigset_t *s, int sig)
{
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }
    *s |= 1UL << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *s, int sig)
{
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }
    *s &= ~(1UL << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *s, int sig)
{
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }
    return (*s >> (sig - 1)) & 1;
}

int socket(int domain, int type, int protocol)
{
    return (int)sc3(__NR_socket, (u32)domain, (u32)type, (u32)protocol);
}

int bind(int fd, const struct sockaddr_in *addr)
{
    return (int)sc2(__NR_bind, (u32)fd, (u32)addr);
}

int connect(int fd, const struct sockaddr_in *addr)
{
    return (int)sc2(__NR_connect, (u32)fd, (u32)addr);
}

int listen(int fd, int backlog)
{
    return (int)sc2(__NR_listen, (u32)fd, (u32)backlog);
}

int accept(int fd, struct sockaddr_in *addr)
{
    return (int)sc2(__NR_accept, (u32)fd, (u32)addr);
}

s32 sendto(int fd, const void *buf, u32 len, const struct sockaddr_in *to)
{
    return sc4(__NR_sendto, (u32)fd, (u32)buf, len, (u32)to);
}

s32 recvfrom(int fd, void *buf, u32 len, struct sockaddr_in *from)
{
    return sc4(__NR_recvfrom, (u32)fd, (u32)buf, len, (u32)from);
}

int shutdown(int fd, int how)
{
    return (int)sc2(__NR_shutdown, (u32)fd, (u32)how);
}

int netctl(int cmd, u32 arg, void *p)
{
    return (int)sc3(__NR_netctl, (u32)cmd, arg, (u32)p);
}

void put_ip(u32 a)
{
    putdec((a >> 24) & 0xff); putch('.');
    putdec((a >> 16) & 0xff); putch('.');
    putdec((a >> 8) & 0xff);  putch('.');
    putdec(a & 0xff);
}

void put_mac(const u8 *m)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 6; i++) {
        if (i) {
            putch(':');
        }
        putch(hex[(m[i] >> 4) & 0xf]);
        putch(hex[m[i] & 0xf]);
    }
}

int open(const char *path, int flags)
{
    return (int)sc2(__NR_open, (u32)path, (u32)flags);
}

int close(int fd)
{
    return (int)sc1(__NR_close, (u32)fd);
}

s32 read(int fd, void *buf, u32 len)
{
    return sc3(__NR_read, (u32)fd, (u32)buf, len);
}

s32 write(int fd, const void *buf, u32 len)
{
    return sc3(__NR_write, (u32)fd, (u32)buf, len);
}

s32 lseek(int fd, s32 offset, int whence)
{
    return sc3(__NR_lseek, (u32)fd, (u32)offset, (u32)whence);
}

int ioctl(int fd, u32 request, u32 arg)
{
    return (int)sc3(__NR_ioctl, (u32)fd, request, arg);
}

int unlink(const char *path)
{
    return (int)sc1(__NR_unlink, (u32)path);
}

int stat(const char *path, struct stat *st)
{
    return (int)sc2(__NR_stat, (u32)path, (u32)st);
}

int getdents(int index, struct dirent *d)
{
    return (int)sc2(__NR_getdents, (u32)index, (u32)d);
}

int uname(struct utsname *u)
{
    return (int)sc1(__NR_uname, (u32)u);
}

time_t time(time_t *t)
{
    return (time_t)sc1(__NR_time, (u32)t);
}

int fsync(int fd)
{
    return (int)sc1(__NR_fsync, (u32)fd);
}

u32 times(void)
{
    return (u32)sc1(__NR_times, 0);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return (int)sc2(__NR_nanosleep, (u32)req, (u32)rem);
}

void msleep(u32 ms)
{
    struct timespec req;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000UL;
    nanosleep(&req, 0);
}

void exit(int status)
{
    sc1(__NR_exit, (u32)status);
    /*
     * exit() does not return: the kernel unwinds all the way back to
     * whoever spawned this program. If it ever does come back, there is
     * nothing left to return to, so stop here rather than run on into
     * whatever called main().
     */
    for (;;) {
    }
}

/* ---------------------------------------------------------------- */

u32 strlen(const char *s)
{
    const char *p = s;

    while (*p) {
        p++;
    }
    return (u32)(p - s);
}

extern char **environ;

/*
 * The environment a program was started with.
 *
 * Read only: there is no setenv, because a change would have nowhere to
 * go -- the environment is a copy made when the program started, and
 * nothing propagates it back to whoever started it. That is true on any
 * Unix; what is missing here is only the ability to pass a changed one
 * on to a further program.
 */
const char *getenv(const char *name)
{
    int i;

    if (!environ) {
        return 0;
    }
    for (i = 0; environ[i]; i++) {
        const char *e = environ[i];
        const char *n = name;

        while (*n && *e && *e != '=' && *n == *e) {
            n++;
            e++;
        }
        if (!*n && *e == '=') {
            return e + 1;
        }
    }
    return 0;
}

u32 inet_aton(const char *s)
{
    u32 a = 0;
    int i;

    for (i = 0; i < 4; i++) {
        int n = 0, digits = 0;

        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s++ - '0');
            digits++;
        }
        if (!digits || n > 255) {
            return 0;
        }
        a = (a << 8) | (u32)n;
        if (i < 3) {
            if (*s != '.') {
                return 0;
            }
            s++;
        }
    }
    return *s == '\0' ? a : 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void *memset(void *dst, int c, u32 n)
{
    u8 *d = dst;

    while (n--) {
        *d++ = (u8)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void putch(char c)
{
    write(STDOUT_FILENO, &c, 1);
}

void puts(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

void eputs(const char *s)
{
    write(STDERR_FILENO, s, strlen(s));
}

void putdec(u32 v)
{
    char buf[12];
    int i = 0;

    if (v == 0) {
        putch('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0) {
        putch(buf[--i]);
    }
}

void puthex(u32 v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        putch(hex[(v >> i) & 0xf]);
    }
}

/*
 * FIONREAD asks the terminal how much could be read without blocking.
 * Asking is the whole point: a plain read() would block until a key
 * arrived, which is no use to a program that wants to keep drawing.
 */
int key_waiting(void)
{
    u32 n = 0;

    if (ioctl(STDIN_FILENO, FIONREAD, (u32)&n) < 0) {
        return 0;
    }
    return n > 0;
}

int chdir(const char *path)
{
    return (int)sc1(__NR_chdir, (u32)path);
}

int getcwd(char *buf, u32 size)
{
    return (int)sc2(__NR_getcwd, (u32)buf, size);
}

int mkdir(const char *path)
{
    return (int)sc1(__NR_mkdir, (u32)path);
}

int rmdir(const char *path)
{
    return (int)sc1(__NR_rmdir, (u32)path);
}

int fstat(int fd, struct stat *st)
{
    return (int)sc2(__NR_fstat, (u32)fd, (u32)st);
}

int access(const char *path, int mode)
{
    return (int)sc2(__NR_access, (u32)path, (u32)mode);
}

int dup(int fd)
{
    return (int)sc1(__NR_dup, (u32)fd);
}

int dup2(int oldfd, int newfd)
{
    return (int)sc2(__NR_dup2, (u32)oldfd, (u32)newfd);
}

int sysinfo(struct sysinfo *si)
{
    return (int)sc1(__NR_sysinfo, (u32)si);
}

/*
 * The break, cached. Linux's libc does the same: the kernel is asked
 * once, and after that sbrk() only has to ask it to move.
 */
static u32 cur_brk;

int brk(void *addr)
{
    u32 got = (u32)sc1(__NR_brk, (u32)addr);

    cur_brk = got;
    /* The kernel says no by handing back the old break. */
    return got == (u32)addr ? 0 : -ENOMEM;
}

void *sbrk(s32 incr)
{
    u32 old, want;

    if (!cur_brk) {
        cur_brk = (u32)sc1(__NR_brk, 0);
    }
    old = cur_brk;
    if (incr == 0) {
        return (void *)old;
    }
    want = old + (u32)incr;
    /* Wrapping past either end of the address space is not a request
     * the kernel should ever see. */
    if ((incr > 0 && want < old) || (incr < 0 && want > old)) {
        return (void *)-1;
    }
    if (brk((void *)want) < 0) {
        return (void *)-1;
    }
    return (void *)old;
}

int isatty(int fd)
{
    struct stat st;

    if (fstat(fd, &st) < 0) {
        return 0;
    }
    return S_ISCHR(st.st_mode) ? 1 : 0;
}
