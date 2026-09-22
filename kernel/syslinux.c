/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * syslinux.c - the rest of Linux's system call interface.
 *
 * The calls this system grew up with are in syscall.c, several with
 * shapes of their own (stat, getdents). These are the ones a C library
 * written for Linux makes instead -- picolibc's libos/linux, in
 * lib/libc -- with Linux/m68k's numbers and Linux's structures, so that
 * the library builds against Linux's headers and needs no port beyond
 * the numbers.
 *
 * Where the machine has nothing to back a call it answers as a Linux
 * box with that feature missing would, not with ENOSYS:
 *
 *   - There is one user, and it is root: every uid and gid is 0.
 *   - FAT has no permissions and no links: chmod succeeds and changes
 *     nothing, readlink says "not a link", symlink is refused.
 *   - Nothing is ever paged out, so mlock and friends succeed, and
 *     madvise and msync have nothing to do.
 *
 * The *at calls understand AT_FDCWD and absolute paths. A path relative
 * to some other open directory is refused with EOPNOTSUPP: a directory
 * descriptor here knows WHICH directory, not where it is, and the
 * filesystem's walk starts from the working directory.
 */
#include "sysint.h"
#include "syscall.h"
#include "vfs.h"
#include "task.h"
#include "signal.h"
#include "timer.h"
#include "random.h"
#include "ptregs.h"
#include "pipe.h"
#include "vm.h"
#include "errno.h"
#include "string.h"

/*
 * The sizes Linux/m68k has for these. A field out of place would not
 * fail to compile or to run; it would quietly hand a C library the
 * wrong numbers, so the sizes are checked here.
 */
_Static_assert(sizeof(struct statx) == 256, "struct statx");
_Static_assert(sizeof(struct siginfo) == 128, "struct siginfo");
_Static_assert(sizeof(struct ucontext) == 532, "struct ucontext");
_Static_assert(sizeof(struct kernel_sigaction) == 20, "kernel_sigaction");
_Static_assert(sizeof(struct rusage) == 72, "struct rusage");
_Static_assert(__builtin_offsetof(struct linux_dirent64, d_name) == 19,
               "linux_dirent64");
_Static_assert(__builtin_offsetof(struct statx, stx_mtime) == 112,
               "statx mtime");
_Static_assert(sizeof(struct timespec) == 8, "32-bit timespec");
_Static_assert(sizeof(struct termios2) == 44, "struct termios2");

/* ---------------------------------------------------------------- */
/* Paths                                                             */
/* ---------------------------------------------------------------- */

/*
 * Fetch a path for an *at call. Returns 0, or -errno. `dirfd` may be
 * AT_FDCWD; any other directory is accepted only if the path does not
 * depend on it.
 */
static int at_path(int dirfd, u32 upath, char *path)
{
    int err = fetch_str(path, upath, PATH_MAX);

    if (err < 0) {
        return err;
    }
    if (!path[0]) {
        return -ENOENT;
    }
    if (dirfd == AT_FDCWD || path[0] == '/') {
        return 0;
    }
    if (!fd_get(dirfd)) {
        return -EBADF;
    }
    if (!vfs_is_dir_file(fd_get(dirfd))) {
        return -ENOTDIR;
    }
    return -EOPNOTSUPP;
}

/* ---------------------------------------------------------------- */
/* statx                                                             */
/* ---------------------------------------------------------------- */

/*
 * From this system's struct stat to Linux's statx. The permission bits
 * are made up the way a Linux FAT mount makes them up: readable by all,
 * writable by the owner if not read-only, and executable if it is a
 * program -- judged by the same four bytes exec() judges it by, so the
 * two never disagree.
 */
static void to_statx(const struct stat *st, int is_prog, struct statx *sx)
{
    u32 mode = st->st_mode;

    memset(sx, 0, sizeof(*sx));
    if (S_ISDIR(mode)) {
        mode = (mode & S_IFMT) | 0755;
    } else if (S_ISREG(mode)) {
        mode = (mode & (S_IFMT | S_IWUSR)) | 0444 | (is_prog ? 0111 : 0);
    } else {
        mode = (mode & S_IFMT) | 0666;
    }

    sx->stx_mask = STATX_BASIC_STATS;
    sx->stx_blksize = 4096;
    sx->stx_nlink = 1;
    sx->stx_mode = (u16)mode;
    sx->stx_ino = st->st_ino ? st->st_ino : 1;
    sx->stx_size = st->st_size;
    sx->stx_blocks = (st->st_size + 511) / 512;
    sx->stx_atime.tv_sec = st->st_mtime;
    sx->stx_btime.tv_sec = st->st_mtime;
    sx->stx_ctime.tv_sec = st->st_mtime;
    sx->stx_mtime.tv_sec = st->st_mtime;
    /* The first partition of the first IDE disk, as Linux numbers it. */
    sx->stx_dev_major = 3;
    sx->stx_dev_minor = 1;
}

static s32 do_statx(int dirfd, u32 upath, int flags, u32 ubuf)
{
    char path[PATH_MAX];
    struct stat st;
    struct statx sx;
    int err, is_prog = 0;

    memset(&st, 0, sizeof(st));

    /* AT_EMPTY_PATH with no path: the descriptor itself. That is how
     * fstat() is built, and it passes a null pointer for the path. */
    if (flags & AT_EMPTY_PATH) {
        if (upath) {
            err = fetch_str(path, upath, PATH_MAX);
            if (err < 0) {
                return err;
            }
        } else {
            path[0] = '\0';
        }
        if (!path[0]) {
            if (dirfd == AT_FDCWD) {
                err = vfs_stat(".", &st);
            } else {
                err = vfs_fstat(dirfd, &st);
            }
            if (err < 0) {
                return err;
            }
            to_statx(&st, 0, &sx);
            return store(ubuf, &sx, sizeof(sx));
        }
    }

    err = at_path(dirfd, upath, path);
    if (err < 0) {
        return err;
    }
    err = vfs_stat(path, &st);
    if (err < 0) {
        return err;
    }
    if (S_ISREG(st.st_mode)) {
        is_prog = vfs_access(path, X_OK) == 0;
    }
    to_statx(&st, is_prog, &sx);
    return store(ubuf, &sx, sizeof(sx));
}

/* ---------------------------------------------------------------- */
/* Directories                                                       */
/* ---------------------------------------------------------------- */

/* Through a kernel buffer, a page at most: a directory read is not the
 * place to be mapping user memory a record at a time. */
static u8 dents_buf[4096];

static s32 do_getdents64(int fd, u32 ubuf, u32 len)
{
    s32 n;

    if (len > sizeof(dents_buf)) {
        len = sizeof(dents_buf);
    }
    n = vfs_getdents64(fd, dents_buf, len);
    if (n <= 0) {
        return n;
    }
    {
        int err = store(ubuf, dents_buf, (u32)n);

        return err < 0 ? err : n;
    }
}

/* ---------------------------------------------------------------- */
/* Time                                                              */
/* ---------------------------------------------------------------- */

static s32 do_clock_gettime(int id, u32 uts)
{
    struct timespec ts;

    switch (id) {
    case CLOCK_REALTIME: {
        struct timeval tv;

        clock_get(&tv);
        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = tv.tv_usec * 1000;
        break;
    }
    case CLOCK_MONOTONIC: {
        /* Ticks since boot: never set, never jumps. */
        u32 j = timer_jiffies();

        ts.tv_sec = j / HZ;
        ts.tv_nsec = (j % HZ) * (1000000000UL / HZ);
        break;
    }
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID: {
        /* One thread per process, so the two are the same clock. */
        u32 j = current->utime + current->stime;

        ts.tv_sec = j / HZ;
        ts.tv_nsec = (j % HZ) * (1000000000UL / HZ);
        break;
    }
    default:
        return -EINVAL;
    }
    return store(uts, &ts, sizeof(ts));
}

/* ---------------------------------------------------------------- */
/* Limits                                                            */
/* ---------------------------------------------------------------- */

/* What each limit really is here, where there is a real answer. */
static u32 limit_of(int res)
{
    switch (res) {
    case RLIMIT_NOFILE:
        return OPEN_MAX;
    case RLIMIT_STACK:
        return USER_STACK_PAGES * PAGE_SIZE;
    case RLIMIT_NPROC:
        return TASK_MAX;
    default:
        return RLIM_INFINITY;
    }
}

static s32 do_getrlimit(int res, u32 ubuf)
{
    struct rlimit r;

    if (res < 0 || res >= RLIM_NLIMITS) {
        return -EINVAL;
    }
    r.rlim_cur = r.rlim_max = limit_of(res);
    return store(ubuf, &r, sizeof(r));
}

/*
 * prlimit64 on this process only. Setting a limit is accepted and has
 * no effect -- every one of them is either a hard fact about the kernel
 * or unlimited -- except that asking for more than the maximum is
 * refused, as it would be for anyone on Linux.
 */
static s32 do_prlimit64(int pid, int res, u32 unew, u32 uold)
{
    struct rlimit64 r;
    u32 max;

    if (pid != 0 && pid != current->pid) {
        return -ESRCH;
    }
    if (res < 0 || res >= RLIM_NLIMITS) {
        return -EINVAL;
    }
    max = limit_of(res);
    if (unew) {
        int err = fetch(&r, unew, sizeof(r));

        if (err < 0) {
            return err;
        }
        if (max != RLIM_INFINITY &&
            (r.rlim_max > max || r.rlim_cur > r.rlim_max)) {
            return -EPERM;
        }
    }
    if (uold) {
        r.rlim_cur = r.rlim_max =
            (max == RLIM_INFINITY) ? ~(u64)0 : (u64)max;
        return store(uold, &r, sizeof(r));
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Signals: the rt_ calls                                            */
/* ---------------------------------------------------------------- */

static s32 do_rt_sigaction(int sig, u32 uact, u32 uold, u32 size)
{
    struct kernel_sigaction ka;
    struct sigaction act, old;
    int err;

    if (size != SIGSET_BYTES) {
        return -EINVAL;
    }
    if (uact) {
        err = fetch(&ka, uact, sizeof(ka));
        if (err < 0) {
            return err;
        }
        act.sa_handler = ka.sa_handler;
        act.sa_flags = ka.sa_flags;
        act.sa_restorer = ka.sa_restorer;
        act.sa_mask = ka.sa_mask[0];
    }
    err = signal_set_action(sig, uact ? &act : 0, &old);
    if (err < 0) {
        return err;
    }
    if (uold) {
        memset(&ka, 0, sizeof(ka));
        ka.sa_handler = old.sa_handler;
        ka.sa_flags = old.sa_flags;
        ka.sa_restorer = old.sa_restorer;
        ka.sa_mask[0] = old.sa_mask;
        return store(uold, &ka, sizeof(ka));
    }
    return 0;
}

static s32 do_rt_sigprocmask(int how, u32 uset, u32 uold, u32 size)
{
    u32 set[2], old[2] = { 0, 0 };
    int err;

    if (size != SIGSET_BYTES) {
        return -EINVAL;
    }
    if (uset) {
        err = fetch(set, uset, sizeof(set));
        if (err < 0) {
            return err;
        }
    }
    err = signal_procmask(how, uset ? &set[0] : 0, &old[0]);
    if (err < 0) {
        return err;
    }
    return uold ? store(uold, old, sizeof(old)) : 0;
}

/* ---------------------------------------------------------------- */
/* The dispatcher                                                    */
/* ---------------------------------------------------------------- */

s32 syscall_linux(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6,
                  struct pt_regs *regs)
{
    char path[PATH_MAX], path2[PATH_MAX];
    int err;

    (void)a6;

    switch (nr) {
    /* --- files and directories --- */
    case __NR_statx:
        return do_statx((int)a1, a2, (int)a3, a5);

    case __NR_lstat: {          /* there are no links: it is stat */
        struct stat st;

        err = fetch_str(path, a1, sizeof(path));
        if (err < 0) {
            return err;
        }
        err = vfs_stat(path, &st);
        return err < 0 ? err : store(a2, &st, sizeof(st));
    }

    case __NR_getdents64:
        return do_getdents64((int)a1, a2, a3);

    case __NR_openat:
        err = at_path((int)a1, a2, path);
        return err < 0 ? err : fd_open(path, (int)a3);

    case __NR_mkdirat:
        err = at_path((int)a1, a2, path);
        return err < 0 ? err : vfs_mkdir(path);

    case __NR_unlinkat:
        if (a3 & ~(u32)AT_REMOVEDIR) {
            return -EINVAL;
        }
        err = at_path((int)a1, a2, path);
        if (err < 0) {
            return err;
        }
        return (a3 & AT_REMOVEDIR) ? vfs_rmdir(path) : vfs_unlink(path);

    case __NR_renameat:
        err = at_path((int)a1, a2, path);
        if (err < 0) {
            return err;
        }
        err = at_path((int)a3, a4, path2);
        return err < 0 ? err : vfs_rename(path, path2);

    case __NR_faccessat:
        err = at_path((int)a1, a2, path);
        return err < 0 ? err : vfs_access(path, (int)a3);

    case __NR_chmod:
    case __NR_fchmodat: {
        struct stat st;

        if (nr == __NR_chmod) {
            err = fetch_str(path, a1, sizeof(path));
        } else {
            err = at_path((int)a1, a2, path);
        }
        return err < 0 ? err : vfs_stat(path, &st);
    }

    case __NR_readlink:
    case __NR_readlinkat: {
        struct stat st;

        if (nr == __NR_readlink) {
            err = fetch_str(path, a1, sizeof(path));
        } else {
            err = at_path((int)a1, a2, path);
        }
        if (err < 0) {
            return err;
        }
        err = vfs_stat(path, &st);
        return err < 0 ? err : -EINVAL;     /* there, and not a link */
    }

    case __NR_symlinkat:
        return -EPERM;          /* FAT cannot hold one */

    case __NR_pipe2: {
        int fds[2];

        err = pipe_create(fds, (int)a2);
        if (err < 0) {
            return err;
        }
        err = store(a1, fds, sizeof(fds));
        if (err < 0) {
            fd_close(fds[0]);
            fd_close(fds[1]);
        }
        return err;
    }

    case __NR_dup3:
        if (a1 == a2) {
            return -EINVAL;
        }
        if (a3 & ~(u32)O_CLOEXEC) {
            return -EINVAL;
        }
        err = fd_dup2((int)a1, (int)a2);
        if (err >= 0 && (a3 & O_CLOEXEC)) {
            fd_fcntl(err, F_SETFD, FD_CLOEXEC);
        }
        return err;

    case __NR__llseek: {
        s32 off = (s32)a3;
        s32 r;
        s64 result;

        /* The high word has to be what a 32-bit offset sign-extends to:
         * no file here is bigger than 4 GB. */
        if ((s32)a2 != (off < 0 ? -1 : 0)) {
            return -EOVERFLOW;
        }
        r = fd_lseek((int)a1, off, (int)a5);
        if (r < 0) {
            return r;
        }
        result = (u32)r;
        return store(a4, &result, sizeof(result));
    }

    case __NR_umask: {
        u32 old = current->umask;

        current->umask = a1 & 0777;
        return (s32)old;
    }

    /* --- identity: one user, root --- */
    case __NR_getuid:  case __NR_getuid32:
    case __NR_geteuid: case __NR_geteuid32:
    case __NR_getgid:  case __NR_getgid32:
    case __NR_getegid: case __NR_getegid32:
        return 0;

    case __NR_setuid:  case __NR_setuid32:
    case __NR_setgid:  case __NR_setgid32:
        return a1 == 0 ? 0 : -EPERM;

    case __NR_setreuid:
    case __NR_setregid:
        return ((s32)a1 <= 0 && (s32)a2 <= 0) ? 0 : -EPERM;

    case __NR_getgroups:
    case __NR_getgroups32:
        return 0;               /* no supplementary groups */

    case __NR_setgroups:
    case __NR_setgroups32:
        return a1 == 0 ? 0 : -EINVAL;

    /* --- processes --- */
    case __NR_wait4: {
        int status = 0;
        int got = task_wait((int)a1, &status, (int)a3);

        if (got < 0) {
            return got;
        }
        if (a2) {
            err = store(a2, &status, sizeof(status));
            if (err < 0) {
                return err;
            }
        }
        if (a4) {
            struct rusage ru;

            memset(&ru, 0, sizeof(ru));
            err = store(a4, &ru, sizeof(ru));
            if (err < 0) {
                return err;
            }
        }
        return got;
    }

    /*
     * vfork is fork: the child gets a copy rather than borrowing the
     * parent's memory, which is always a correct implementation of it.
     * clone only as fork, with nothing shared and SIGCHLD at exit.
     */
    case __NR_vfork:
    case __NR_clone: {
        struct task *t;

        if (nr == __NR_clone && (a1 != SIGCHLD || a2 != 0)) {
            return -EINVAL;
        }
        t = task_fork(regs);
        return t ? t->pid : -ENOMEM;
    }

    /* --- time, limits, randomness --- */
    case __NR_clock_gettime:
        return do_clock_gettime((int)a1, a2);

    case __NR_getrlimit:
    case __NR_ugetrlimit:
        return do_getrlimit((int)a1, a2);

    case __NR_setrlimit: {
        struct rlimit r;
        u32 max;

        if ((int)a1 < 0 || (int)a1 >= RLIM_NLIMITS) {
            return -EINVAL;
        }
        err = fetch(&r, a2, sizeof(r));
        if (err < 0) {
            return err;
        }
        max = limit_of((int)a1);
        if (max != RLIM_INFINITY &&
            (r.rlim_max > max || r.rlim_cur > r.rlim_max)) {
            return -EPERM;
        }
        return 0;
    }

    case __NR_prlimit64:
        return do_prlimit64((int)a1, (int)a2, a3, a4);

    case __NR_getrandom: {
        u32 done = 0;

        if (a3 & ~(u32)(GRND_NONBLOCK | GRND_RANDOM)) {
            return -EINVAL;
        }
        while (done < a2) {
            u32 v = random_u32();
            u32 n = (a2 - done < 4) ? a2 - done : 4;

            err = store(a1 + done, &v, n);
            if (err < 0) {
                return done ? (s32)done : err;
            }
            done += n;
        }
        return (s32)done;
    }

    /* --- memory: nothing is ever paged out --- */
    case __NR_madvise:
    case __NR_msync:
    case __NR_mlock:
    case __NR_munlock:
    case __NR_mlockall:
    case __NR_munlockall:
        return 0;

    /* --- signals --- */
    case __NR_rt_sigaction:
        return do_rt_sigaction((int)a1, a2, a3, a4);

    case __NR_rt_sigprocmask:
        return do_rt_sigprocmask((int)a1, a2, a3, a4);

    case __NR_rt_sigpending: {
        u32 set[2];

        if (a2 != SIGSET_BYTES) {
            return -EINVAL;
        }
        set[0] = signal_pending_set();
        set[1] = 0;
        return store(a1, set, sizeof(set));
    }

    case __NR_rt_sigsuspend: {
        u32 set[2];

        if (a2 != SIGSET_BYTES) {
            return -EINVAL;
        }
        err = fetch(set, a1, sizeof(set));
        return err < 0 ? err : signal_suspend(set[0]);
    }

    case __NR_rt_sigreturn:
        return signal_rt_return(regs);

    default:
        return -ENOSYS;
    }
}
