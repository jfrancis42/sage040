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
