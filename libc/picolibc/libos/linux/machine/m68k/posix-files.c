/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright © 2026 Jeff Francis
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE             /* every declaration picolibc has */

/*
 * File calls POSIX has and picolibc's libos/linux (1.8.12) does not:
 * fdopendir and dirfd, the rest of the *at family, file times (utimensat,
 * futimens, utime, utimes), sync, and mkdtemp. Nothing here is specific
 * to m68k beyond the kernel's structures; it lives in the m68k backend
 * so the release underneath stays unmodified.
 *
 * picolibc's AT_ and O_ constants are not Linux's: see posix-more.c.
 */

#include "../../local-linux.h"
#include "../../local-dirent.h"
#include "../../local-time.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <unistd.h>

static int
at_dirfd(int dirfd)
{
    return dirfd == AT_FDCWD ? (int)LINUX_AT_FDCWD : dirfd;
}

/* ---- directories ------------------------------------------------ */

DIR *
fdopendir(int fd)
{
    struct stat st;
    DIR        *dir;

    if (fstat(fd, &st) < 0)
        return NULL;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    dir = calloc(1, sizeof(DIR));
    if (!dir)
        return NULL;
    dir->fd = fd;                   /* closedir() closes it, as POSIX says */
    return dir;
}

int
dirfd(DIR *dir)
{
    return dir->fd;
}

/* ---- the rest of the *at family ------------------------------------ */

int
fchownat(int dirfd, const char *path, uid_t owner, gid_t group, int flags)
{
    if (flags & ~AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_fchownat, at_dirfd(dirfd), path, owner, group,
                   (flags & AT_SYMLINK_NOFOLLOW) ? LINUX_AT_SYMLINK_NOFOLLOW : 0);
}

/* Linux's fchmodat takes no flags; AT_SYMLINK_NOFOLLOW is meaningless
 * with no links to follow. */
int
fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    if (flags & ~AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_fchmodat, at_dirfd(dirfd), path, mode);
}

int
linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
    (void)flags;
    return syscall(LINUX_SYS_linkat, at_dirfd(olddirfd), oldpath, at_dirfd(newdirfd),
                   newpath, 0);
}

int
symlinkat(const char *target, int dirfd, const char *path)
{
    return syscall(LINUX_SYS_symlinkat, target, at_dirfd(dirfd), path);
}

ssize_t
readlinkat(int dirfd, const char *__restrict path, char *__restrict buf, size_t len)
{
    return syscall(LINUX_SYS_readlinkat, at_dirfd(dirfd), path, buf, len);
}

int
renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
    return syscall(LINUX_SYS_renameat, at_dirfd(olddirfd), oldpath, at_dirfd(newdirfd),
                   newpath);
}

/* ---- file times ------------------------------------------------- */

/*
 * The kernel takes Linux/m68k's two 32-bit timespecs; picolibc's have a
 * 64-bit tv_sec. UTIME_NOW and UTIME_OMIT are Linux's values in both.
 */
int
utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    struct __kernel_timespec k[2];
    int                      i;

    if (flags & ~AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    if (times) {
        for (i = 0; i < 2; i++) {
            if (times[i].tv_nsec != UTIME_NOW && times[i].tv_nsec != UTIME_OMIT &&
                (times[i].tv_sec < 0 || times[i].tv_sec > 0xffffffffLL)) {
                errno = EINVAL;
                return -1;
            }
            k[i].tv_sec = (__int32_t)times[i].tv_sec;
            k[i].tv_nsec = (__int32_t)times[i].tv_nsec;
        }
    }
    return syscall(LINUX_SYS_utimensat, path ? at_dirfd(dirfd) : dirfd, path,
                   times ? k : NULL,
                   (flags & AT_SYMLINK_NOFOLLOW) ? LINUX_AT_SYMLINK_NOFOLLOW : 0);
}

int
futimens(int fd, const struct timespec times[2])
{
    return utimensat(fd, NULL, times, 0);
}

int
utime(const char *path, const struct utimbuf *times)
{
    struct timespec ts[2];

    if (!times)
        return utimensat(AT_FDCWD, path, NULL, 0);
    ts[0].tv_sec = times->actime;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = times->modtime;
    ts[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, path, ts, 0);
}

int
utimes(const char *path, const struct timeval times[2])
{
    struct timespec ts[2];
    int             i;

    if (!times)
        return utimensat(AT_FDCWD, path, NULL, 0);
    for (i = 0; i < 2; i++) {
        ts[i].tv_sec = times[i].tv_sec;
        ts[i].tv_nsec = times[i].tv_usec * 1000;
    }
    return utimensat(AT_FDCWD, path, ts, 0);
}

/* ---- the rest ----------------------------------------------------- */

int
chroot(const char *path)
{
    return syscall(LINUX_SYS_chroot, path);
}

void
sync(void)
{
    (void)syscall(LINUX_SYS_sync);
}

/* A directory with a unique name made from TEMPLATE's trailing XXXXXX. */
char *
mkdtemp(char *template)
{
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t            n = strlen(template);
    char             *x;
    int               tries, i;

    if (n < 6 || strcmp(template + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return NULL;
    }
    x = template + n - 6;
    for (tries = 0; tries < 1000; tries++) {
        for (i = 0; i < 6; i++)
            x[i] = chars[arc4random_uniform(sizeof(chars) - 1)];
        if (mkdir(template, 0700) == 0)
            return template;
        if (errno != EEXIST)
            return NULL;
    }
    errno = EEXIST;
    return NULL;
}

/* ---- descriptors -------------------------------------------------- */

/* The two flags pipe2 and dup3 take, picolibc's to Linux's. */
static int
fd_flags_to_linux(int flags, int allowed)
{
    int l = 0;

    if (flags & ~allowed)
        return -1;
    if (flags & O_CLOEXEC)
        l |= LINUX_O_CLOEXEC;
    if (flags & O_NONBLOCK)
        l |= LINUX_O_NONBLOCK;
    return l;
}

int
pipe2(int fds[2], int flags)
{
    int l = fd_flags_to_linux(flags, O_CLOEXEC | O_NONBLOCK);

    if (l < 0) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_pipe2, fds, l);
}

int
dup3(int oldfd, int newfd, int flags)
{
    int l = fd_flags_to_linux(flags, O_CLOEXEC);

    if (l < 0) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_dup3, oldfd, newfd, l);
}

/* ---- FIFOs and device nodes, at a directory ------------------------ */

/* FAT can hold neither, and the kernel says so (EPERM); these are
 * complete all the same. */
int
mknodat(int dirfd, const char *path, mode_t mode, dev_t dev)
{
    unsigned __sage040_linux_dev(dev_t dev);    /* posix-more.c */

    return syscall(LINUX_SYS_mknodat, at_dirfd(dirfd), path, mode, __sage040_linux_dev(dev));
}

int
mkfifoat(int dirfd, const char *path, mode_t mode)
{
    return mknodat(dirfd, path, (mode & 07777) | S_IFIFO, 0);
}
