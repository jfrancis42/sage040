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
 * Process and system calls POSIX (and Linux) have and picolibc's
 * libos/linux (1.8.12) does not: sessions, the host name, priorities
 * (getpriority, setpriority, nice), sigaltstack, getrandom, getloadavg,
 * confstr, clock_settime, and posix_spawn with its file actions and
 * attributes. As with posix-more.c, nothing here is specific to m68k
 * but the structures, and it lives here so the release underneath
 * stays unmodified.
 */

#include "../../local-linux.h"
#include "../../local-sigaction.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ---- sessions and the host name ------------------------------------ */

pid_t
setsid(void)
{
    return syscall(LINUX_SYS_setsid);
}

pid_t
getsid(pid_t pid)
{
    return syscall(LINUX_SYS_getsid, pid);
}

int
sethostname(const char *name, size_t len)
{
    return syscall(LINUX_SYS_sethostname, name, len);
}

/*
 * daemon(): into the background as the BSDs and glibc do it -- fork and
 * let the parent go, a session of its own, / as the working directory
 * unless nochdir, and stdin, stdout and stderr on /dev/null unless
 * noclose.
 */
int
daemon(int nochdir, int noclose)
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    if (setsid() < 0)
        return -1;
    if (!nochdir && chdir("/") < 0)
        return -1;
    if (!noclose) {
        int fd = open("/dev/null", O_RDWR);

        if (fd < 0)
            return -1;
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
    return 0;
}

/* ---- priorities --------------------------------------------------- */

/*
 * Linux's getpriority system call returns 20 - nice, so that no valid
 * answer looks like an error; the C library turns it back.
 */
int
getpriority(int which, id_t who)
{
    int r = syscall(LINUX_SYS_getpriority, which, who);

    return r < 0 ? r : 20 - r;
}

int
setpriority(int which, id_t who, int prio)
{
    return syscall(LINUX_SYS_setpriority, which, who, prio);
}

/* nice() returns the new value, and -1 with errno set on failure; -1 is
 * also a valid value, so errno is cleared first, as POSIX asks. */
int
nice(int incr)
{
    int cur;

    errno = 0;
    cur = getpriority(PRIO_PROCESS, 0);
    if (cur == -1 && errno)
        return -1;
    if (setpriority(PRIO_PROCESS, 0, cur + incr) < 0) {
        if (errno == EACCES)
            errno = EPERM;
        return -1;
    }
    return getpriority(PRIO_PROCESS, 0);
}

/* ---- signals ------------------------------------------------------ */

/* stack_t is laid out as Linux's; SS_ONSTACK and SS_DISABLE are too. */
int
sigaltstack(const stack_t *__restrict ss, stack_t *__restrict old)
{
    return syscall(LINUX_SYS_sigaltstack, ss, old);
}

/* ---- randomness, load, configuration, the clock ------------------ */

ssize_t
getrandom(void *buf, size_t len, unsigned int flags)
{
    return syscall(LINUX_SYS_getrandom, buf, len, flags);
}

/* Linux's sysinfo carries the load averages scaled by 65536. */
int
getloadavg(double loadavg[], int nelem)
{
    struct {
        __int32_t uptime;
        __uint32_t loads[3];
        __uint32_t rest[13];
    } si;
    int i;

    if (nelem < 0) {
        errno = EINVAL;
        return -1;
    }
    if (syscall(LINUX_SYS_sysinfo, &si) < 0)
        return -1;
    if (nelem > 3)
        nelem = 3;
    for (i = 0; i < nelem; i++)
        loadavg[i] = si.loads[i] / 65536.0;
    return nelem;
}

size_t
confstr(int name, char *buf, size_t len)
{
    const char *v;
    size_t      n;

    switch (name) {
    case _CS_PATH:
        v = "/bin";             /* where the standard utilities are */
        break;
    default:
        errno = EINVAL;
        return 0;
    }
    n = strlen(v) + 1;
    if (buf && len) {
        strncpy(buf, v, len - 1);
        buf[len - 1] = '\0';
    }
    return n;
}

/* Only the real-time clock can be set; to the microsecond, through
 * settimeofday, whose 32-bit timeval is Linux/m68k's. */
int
clock_settime(clockid_t id, const struct timespec *ts)
{
    struct {
        __int32_t tv_sec;
        __int32_t tv_usec;
    } ktv;

    if (id != CLOCK_REALTIME) {
        errno = EINVAL;
        return -1;
    }
    if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000L || ts->tv_sec < 0 ||
        ts->tv_sec > 0xffffffffLL) {
        errno = EINVAL;
        return -1;
    }
    ktv.tv_sec = (__int32_t)ts->tv_sec;
    ktv.tv_usec = (__int32_t)(ts->tv_nsec / 1000);
    return syscall(LINUX_SYS_settimeofday, &ktv, NULL);
}

/* ---- posix_spawn ------------------------------------------------- */

/*
 * fork, do the file actions and attributes in the child, exec. vfork
 * here is fork, so there is nothing to gain from it. An exec that fails
 * reports its errno to the parent through a close-on-exec pipe -- the
 * pipe closing unread is the sign the exec worked -- so posix_spawn
 * itself returns the error, as POSIX lets it.
 */
enum { FA_OPEN, FA_DUP2, FA_CLOSE, FA_CHDIR, FA_FCHDIR };

struct fa {
    struct fa *next;
    int        what, fd, newfd, oflag;
    mode_t     mode;
    char      *path;
};

struct __posix_spawn_file_actions {
    struct fa *head, *tail;
};

struct __posix_spawnattr {
    short    flags;
    pid_t    pgroup;
    sigset_t sigdefault;
    sigset_t sigmask;
};

int
posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    *fa = calloc(1, sizeof(**fa));
    return *fa ? 0 : ENOMEM;
}

int
posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    struct fa *a, *n;

    for (a = (*fa)->head; a; a = n) {
        n = a->next;
        free(a->path);
        free(a);
    }
    free(*fa);
    *fa = NULL;
    return 0;
}

static int
fa_add(posix_spawn_file_actions_t *fa, int what, int fd, int newfd, const char *path,
       int oflag, mode_t mode)
{
    struct fa *a;

    if (fd < 0 || newfd < 0)
        return EBADF;
    a = calloc(1, sizeof(*a));
    if (!a)
        return ENOMEM;
    a->what = what;
    a->fd = fd;
    a->newfd = newfd;
    a->oflag = oflag;
    a->mode = mode;
    if (path && !(a->path = strdup(path))) {
        free(a);
        return ENOMEM;
    }
    if ((*fa)->tail)
        (*fa)->tail->next = a;
    else
        (*fa)->head = a;
    (*fa)->tail = a;
    return 0;
}

int
posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *__restrict fa, int fd,
                                 const char *__restrict path, int oflag, mode_t mode)
{
    return fa_add(fa, FA_OPEN, fd, 0, path, oflag, mode);
}

int
posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd)
{
    return fa_add(fa, FA_DUP2, fd, newfd, NULL, 0, 0);
}

int
posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
    return fa_add(fa, FA_CLOSE, fd, 0, NULL, 0, 0);
}

int
posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *__restrict fa,
                                  const char *__restrict path)
{
    return fa_add(fa, FA_CHDIR, 0, 0, path, 0, 0);
}

int
posix_spawn_file_actions_addfchdir(posix_spawn_file_actions_t *__restrict fa, int fd)
{
    return fa_add(fa, FA_FCHDIR, fd, 0, NULL, 0, 0);
}

int
posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *__restrict fa,
                                     const char *__restrict path)
{
    return posix_spawn_file_actions_addchdir(fa, path);
}

int
posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t *__restrict fa, int fd)
{
    return posix_spawn_file_actions_addfchdir(fa, fd);
}

int
posix_spawnattr_init(posix_spawnattr_t *attr)
{
    *attr = calloc(1, sizeof(**attr));
    return *attr ? 0 : ENOMEM;
}

int
posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    free(*attr);
    *attr = NULL;
    return 0;
}

int
posix_spawnattr_getflags(const posix_spawnattr_t *__restrict attr, short *__restrict flags)
{
    *flags = (*attr)->flags;
    return 0;
}

int
posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    if (flags & ~(POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                  POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSCHEDPARAM |
                  POSIX_SPAWN_SETSCHEDULER))
        return EINVAL;
    (*attr)->flags = flags;
    return 0;
}

int
posix_spawnattr_getpgroup(const posix_spawnattr_t *__restrict attr, pid_t *__restrict pg)
{
    *pg = (*attr)->pgroup;
    return 0;
}

int
posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pg)
{
    (*attr)->pgroup = pg;
    return 0;
}

int
posix_spawnattr_getsigdefault(const posix_spawnattr_t *__restrict attr,
                              sigset_t *__restrict set)
{
    *set = (*attr)->sigdefault;
    return 0;
}

int
posix_spawnattr_setsigdefault(posix_spawnattr_t *__restrict attr,
                              const sigset_t *__restrict set)
{
    (*attr)->sigdefault = *set;
    return 0;
}

int
posix_spawnattr_getsigmask(const posix_spawnattr_t *__restrict attr, sigset_t *__restrict set)
{
    *set = (*attr)->sigmask;
    return 0;
}

int
posix_spawnattr_setsigmask(posix_spawnattr_t *__restrict attr, const sigset_t *__restrict set)
{
    (*attr)->sigmask = *set;
    return 0;
}

/* There is one scheduling policy and no per-process parameters to set:
 * asking for them is refused rather than ignored. */
int
posix_spawnattr_getschedparam(const posix_spawnattr_t *__restrict attr,
                              struct sched_param *__restrict p)
{
    (void)attr;
    (void)p;
    return ENOSYS;
}

int
posix_spawnattr_setschedparam(posix_spawnattr_t *__restrict attr,
                              const struct sched_param *__restrict p)
{
    (void)attr;
    (void)p;
    return ENOSYS;
}

int
posix_spawnattr_getschedpolicy(const posix_spawnattr_t *__restrict attr, int *__restrict p)
{
    (void)attr;
    (void)p;
    return ENOSYS;
}

int
posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int p)
{
    (void)attr;
    (void)p;
    return ENOSYS;
}

static int
spawn(pid_t *pidp, const char *path, const posix_spawn_file_actions_t *fa,
      const posix_spawnattr_t *attr, char *const argv[], char *const envp[], int search)
{
    int   p[2], err = 0, i;
    pid_t pid;
    short flags = attr && *attr ? (*attr)->flags : 0;

    if (flags & (POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER))
        return ENOSYS;
    if (pipe2(p, O_CLOEXEC) < 0)
        return errno;
    pid = fork();
    if (pid < 0) {
        err = errno;
        close(p[0]);
        close(p[1]);
        return err;
    }
    if (pid == 0) {
        struct fa *a;

        close(p[0]);
        if (flags & POSIX_SPAWN_SETPGROUP) {
            if (setpgid(0, (*attr)->pgroup) < 0)
                goto fail;
        }
        if (flags & POSIX_SPAWN_SETSIGDEF) {
            for (i = 1; i < NSIG; i++)
                if (sigismember(&(*attr)->sigdefault, i) == 1)
                    signal(i, SIG_DFL);
        }
        if (flags & POSIX_SPAWN_SETSIGMASK)
            sigprocmask(SIG_SETMASK, &(*attr)->sigmask, NULL);
        for (a = fa && *fa ? (*fa)->head : NULL; a; a = a->next) {
            int fd;

            switch (a->what) {
            case FA_OPEN:
                fd = open(a->path, a->oflag, a->mode);
                if (fd < 0)
                    goto fail;
                if (fd != a->fd) {
                    if (dup2(fd, a->fd) < 0)
                        goto fail;
                    close(fd);
                }
                break;
            case FA_DUP2:
                if (a->fd == a->newfd) {
                    /* POSIX: dup2 to itself clears FD_CLOEXEC. */
                    if (fcntl(a->fd, F_SETFD, 0) < 0)
                        goto fail;
                } else if (dup2(a->fd, a->newfd) < 0) {
                    goto fail;
                }
                break;
            case FA_CLOSE:
                close(a->fd);
                break;
            case FA_CHDIR:
                if (chdir(a->path) < 0)
                    goto fail;
                break;
            case FA_FCHDIR:
                if (fchdir(a->fd) < 0)
                    goto fail;
                break;
            }
        }
        if (search)
            execvpe(path, argv, envp);
        else
            execve(path, argv, envp);
    fail:
        err = errno;
        write(p[1], &err, sizeof(err));
        _exit(127);
    }
    close(p[1]);
    if (read(p[0], &err, sizeof(err)) == (ssize_t)sizeof(err)) {
        /* The child never became the program: reap it now. */
        waitpid(pid, NULL, 0);
    } else {
        err = 0;
        if (pidp)
            *pidp = pid;
    }
    close(p[0]);
    return err;
}

int
posix_spawn(pid_t *__restrict pid, const char *__restrict path,
            const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *__restrict attr,
            char *const argv[], char *const envp[])
{
    return spawn(pid, path, fa, attr, argv, envp ? envp : environ, 0);
}

int
posix_spawnp(pid_t *__restrict pid, const char *__restrict file,
             const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *__restrict attr,
             char *const argv[], char *const envp[])
{
    return spawn(pid, file, fa, attr, argv, envp ? envp : environ, 1);
}
