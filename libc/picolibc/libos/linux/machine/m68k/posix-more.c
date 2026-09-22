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

/*
 * More of POSIX that picolibc's libos/linux (1.8.12) lacks on every
 * architecture, found by setting out to build bash, sed, grep and awk:
 * uname, wait4, getrusage, fchmod, fchdir, the chown family, link,
 * mknod and mkfifo, the *at calls (openat, mkdirat, fstatat, unlinkat,
 * faccessat), realpath and getdtablesize. As with
 * posix-extra.c, nothing here is specific to m68k; it lives in the m68k
 * backend only so that the release underneath stays unmodified.
 *
 * Two things need converting on the way through:
 *
 *   - picolibc's AT_ constants are not Linux's (AT_FDCWD is -2 here and
 *     -100 there, AT_REMOVEDIR 8 against 0x200), and neither are its O_
 *     flags, so every *at call translates.
 *   - Its struct rusage holds 64-bit-seconds timevals and the kernel's
 *     holds 32-bit ones, so rusage is converted field by field.
 */

#include "../../local-linux.h"
#include "../../local-statx.h"
#include "../../local-sigaction.h"
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- conversions ------------------------------------------------ */

static int
at_dirfd(int dirfd)
{
    return dirfd == AT_FDCWD ? (int)LINUX_AT_FDCWD : dirfd;
}

/* Linux/m68k's struct rusage: two 32-bit timevals, then fourteen longs. */
struct linux_rusage {
    __int32_t utime_sec, utime_usec;
    __int32_t stime_sec, stime_usec;
    __int32_t other[14];
};

static void
rusage_from_linux(struct rusage *out, const struct linux_rusage *k)
{
    memset(out, 0, sizeof(*out));
    out->ru_utime.tv_sec = k->utime_sec;
    out->ru_utime.tv_usec = k->utime_usec;
    out->ru_stime.tv_sec = k->stime_sec;
    out->ru_stime.tv_usec = k->stime_usec;
}

/* picolibc's O_ flags to Linux's -- the same translation open() makes. */
static int
open_flags_to_linux(int flags)
{
    int l = flags & O_ACCMODE;

    if (flags & O_APPEND)    l |= LINUX_O_APPEND;
    if (flags & O_CLOEXEC)   l |= LINUX_O_CLOEXEC;
    if (flags & O_CREAT)     l |= LINUX_O_CREAT;
    if (flags & O_DIRECTORY) l |= LINUX_O_DIRECTORY;
    if (flags & O_EXCL)      l |= LINUX_O_EXCL;
    if (flags & O_NOCTTY)    l |= LINUX_O_NOCTTY;
    if (flags & O_NOFOLLOW)  l |= LINUX_O_NOFOLLOW;
    if (flags & O_NONBLOCK)  l |= LINUX_O_NONBLOCK;
    if (flags & O_SYNC)      l |= LINUX_O_SYNC;
    if (flags & O_TRUNC)     l |= LINUX_O_TRUNC;
    return l | LINUX_O_LARGEFILE;
}

/* ---- the machine ------------------------------------------------ */

/*
 * The kernel's uname has this system's own shape (the one lib/ulib
 * uses), like its stat and getdents: four fields and no host name. It
 * is widened here to POSIX's. There is no sethostname, so the node
 * name is the machine's.
 */
int
uname(struct utsname *u)
{
    struct {
        char sysname[16];
        char release[16];
        char machine[16];
        char version[32];
    } k;
    int ret = syscall(LINUX_SYS_uname, &k);

    if (ret < 0)
        return ret;
    /* The kernel's fields need not end in a NUL; POSIX's must. */
    memset(u, 0, sizeof(*u));
    memcpy(u->sysname, k.sysname, sizeof(k.sysname));
    strcpy(u->nodename, "sage040");
    memcpy(u->release, k.release, sizeof(k.release));
    memcpy(u->version, k.version, sizeof(k.version));
    memcpy(u->machine, k.machine, sizeof(k.machine));
    return 0;
}

int
getdtablesize(void)
{
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) < 0 || rl.rlim_cur > INT_MAX)
        return 64;
    return (int)rl.rlim_cur;
}

/* ---- processes -------------------------------------------------- */

int
getrusage(int who, struct rusage *usage)
{
    struct linux_rusage k;
    int ret = syscall(LINUX_SYS_getrusage, who, &k);

    if (ret >= 0)
        rusage_from_linux(usage, &k);
    return ret;
}

pid_t
wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage)
{
    int koptions = 0;
    int kstatus = 0;
    struct linux_rusage k;
    pid_t ret;

    if (options & WNOHANG)
        koptions |= LINUX_WNOHANG;
    if (options & WUNTRACED)
        koptions |= LINUX_WUNTRACED;
    if (options & WCONTINUED)
        koptions |= LINUX_WCONTINUED;
    ret = syscall(LINUX_SYS_wait4, pid, &kstatus, koptions, rusage ? &k : NULL);
    if (ret > 0) {
        if (WIFSIGNALED(kstatus))
            kstatus = __W_EXITCODE(0, _signal_from_linux(WTERMSIG(kstatus)));
        else if (WIFSTOPPED(kstatus))
            kstatus = __W_EXITCODE(_signal_from_linux(WSTOPSIG(kstatus)), 0x7f);
        if (wstatus)
            *wstatus = kstatus;
        if (rusage)
            rusage_from_linux(rusage, &k);
    }
    return ret;
}

/* ---- files ------------------------------------------------------ */

int
fchdir(int fd)
{
    return syscall(LINUX_SYS_fchdir, fd);
}

int
fchmod(int fd, mode_t mode)
{
    return syscall(LINUX_SYS_fchmod, fd, mode);
}

int
chown(const char *path, uid_t owner, gid_t group)
{
    return syscall(LINUX_SYS_chown32, path, owner, group);
}

int
lchown(const char *path, uid_t owner, gid_t group)
{
    return syscall(LINUX_SYS_lchown32, path, owner, group);
}

int
fchown(int fd, uid_t owner, gid_t group)
{
    return syscall(LINUX_SYS_fchown32, fd, owner, group);
}

int
link(const char *from, const char *to)
{
    return syscall(LINUX_SYS_link, from, to);
}

int
mknod(const char *path, mode_t mode, dev_t dev)
{
    return syscall(LINUX_SYS_mknod, path, mode, (unsigned)dev);
}

int
mkfifo(const char *path, mode_t mode)
{
    return mknod(path, (mode & 07777) | S_IFIFO, 0);
}

int
openat(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    int mode;

    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    return syscall(LINUX_SYS_openat, at_dirfd(dirfd), path,
                   open_flags_to_linux(flags), mode);
}

int
mkdirat(int dirfd, const char *path, mode_t mode)
{
    return syscall(LINUX_SYS_mkdirat, at_dirfd(dirfd), path, mode);
}

int
fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    struct __kernel_statx sx;
    int kflags = LINUX_AT_STATX_SYNC_AS_STAT;
    int ret;

    if (flags & AT_SYMLINK_NOFOLLOW)
        kflags |= LINUX_AT_SYMLINK_NOFOLLOW;
    ret = syscall(LINUX_SYS_statx, at_dirfd(dirfd), path, kflags,
                  LINUX_STATX_BASIC_STATS, &sx);
    if (ret < 0)
        return ret;
    return _statbuf(st, &sx);
}

int
unlinkat(int dirfd, const char *path, int flags)
{
    if (flags & ~AT_REMOVEDIR) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_unlinkat, at_dirfd(dirfd), path,
                   (flags & AT_REMOVEDIR) ? LINUX_AT_REMOVEDIR : 0);
}

/*
 * AT_EACCESS asks about the effective ids rather than the real ones;
 * with one user they are the same, so it changes nothing here.
 */
int
faccessat(int dirfd, const char *path, int mode, int flags)
{
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) {
        errno = EINVAL;
        return -1;
    }
    return syscall(LINUX_SYS_faccessat, at_dirfd(dirfd), path, mode);
}

/*
 * The absolute name of `path`, with ".", ".." and repeated slashes
 * resolved. There are no symbolic links on this system (symlink is
 * refused), so this is getcwd, then the path, walked textually -- and
 * each prefix checked to exist and, but for the last, to be a directory,
 * which is the part a textual walk alone would get wrong.
 */
char *
realpath(const char *path, char *resolved)
{
    char buf[PATH_MAX];
    const char *p = path;
    size_t n;
    struct stat st;

    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (!path[0]) {
        errno = ENOENT;
        return NULL;
    }
    if (path[0] == '/') {
        strcpy(buf, "/");
    } else if (!getcwd(buf, sizeof(buf))) {
        return NULL;
    }
    n = strlen(buf);

    while (*p) {
        const char *e;
        size_t len;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        for (e = p; *e && *e != '/'; e++)
            ;
        len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') {
            /* nothing */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 1 && buf[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
            buf[n] = '\0';
        } else {
            if (n + (n > 1) + len + 1 > sizeof(buf)) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            if (n > 1)
                buf[n++] = '/';
            memcpy(buf + n, p, len);
            n += len;
            buf[n] = '\0';
            if (stat(buf, &st) < 0)
                return NULL;
            if (*e && !S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                return NULL;
            }
        }
        p = e;
    }

    if (!resolved) {
        resolved = malloc(n + 1);
        if (!resolved)
            return NULL;
    }
    memcpy(resolved, buf, n + 1);
    return resolved;
}

/* ---- limits ----------------------------------------------------- */

/*
 * What this system really has, where the machine knows, and POSIX's
 * minimum where there is nothing more to say. picolibc's only sysconf
 * is libos/fallback's, which answers every question with the minimum --
 * 20 open files, where the limit here is 64 -- and is not in libc.so at
 * all.
 */
long
sysconf(int name)
{
    switch (name) {
    case _SC_ARG_MAX:
        /* kernel/syscall.c: 32 KB for the strings and the 2 x 257
         * pointer slots together. (There is also a 256-argument cap,
         * which POSIX has no name for.) */
        return 32768 - 2 * 257 * 4;
    case _SC_CHILD_MAX:
        /* TASK_MAX, kernel/task.h: picolibc has no RLIMIT_NPROC to ask. */
        return 64;
    case _SC_OPEN_MAX:
    case _SC_STREAM_MAX:
        return getdtablesize();
    case _SC_CLK_TCK:
        return 100;                     /* HZ, kernel/uapi.h */
    case _SC_PAGESIZE:
        return 4096;
    case _SC_NGROUPS_MAX:
        return 0;                       /* no supplementary groups */
    case _SC_HOST_NAME_MAX:
        return 64;
    case _SC_LOGIN_NAME_MAX:
        return 32;
    case _SC_TTY_NAME_MAX:
        return 32;
    case _SC_TZNAME_MAX:
        return 6;
    case _SC_SYMLOOP_MAX:
        return 8;
    case _SC_IOV_MAX:
        return 1024;
    case _SC_LINE_MAX:
        return 2048;
    case _SC_RE_DUP_MAX:
        return 255;
    case _SC_EXPR_NEST_MAX:
        return 32;
    case _SC_COLL_WEIGHTS_MAX:
        return 2;
    case _SC_CHARCLASS_NAME_MAX:
        return 14;
    case _SC_BC_BASE_MAX:
        return 99;
    case _SC_BC_DIM_MAX:
        return 2048;
    case _SC_BC_SCALE_MAX:
        return 99;
    case _SC_BC_STRING_MAX:
        return 1000;
    case _SC_ATEXIT_MAX:
        return 32;
    case _SC_GETPW_R_SIZE_MAX:
    case _SC_GETGR_R_SIZE_MAX:
        return 1024;
    case _SC_VERSION:
    case _SC_2_VERSION:
        return 200809L;
    case _SC_JOB_CONTROL:
    case _SC_SAVED_IDS:
    case _SC_REGEXP:
    case _SC_SHELL:
        return 1;
    case _SC_MONOTONIC_CLOCK:
    case _SC_CPUTIME:
    case _SC_MAPPED_FILES:
    case _SC_MEMORY_PROTECTION:
    case _SC_FSYNC:
    case _SC_TIMEOUTS:
        return 200809L;
    default:
        errno = EINVAL;
        return -1;
    }
}

/* ---- the program's name ----------------------------------------- */

/*
 * crt0 hands argv[0] here before main (libc/crt0.s, crt0-dyn.s). glibc's
 * two names, which GNU programs print in their messages, and the BSDs'
 * getprogname(), which gnulib looks for first.
 */
char *program_invocation_name = "";
char *program_invocation_short_name = "";

void __sage040_progname(char *argv0);

void
__sage040_progname(char *argv0)
{
    char *slash;

    if (!argv0)
        return;
    program_invocation_name = argv0;
    slash = strrchr(argv0, '/');
    program_invocation_short_name = slash ? slash + 1 : argv0;
}

const char *
getprogname(void)
{
    return program_invocation_short_name;
}

void
setprogname(const char *name)
{
    const char *slash = strrchr(name, '/');

    program_invocation_short_name = (char *)(slash ? slash + 1 : name);
}
