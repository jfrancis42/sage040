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
 * The *at calls take AT_FDCWD, absolute paths, and paths relative to
 * an open directory, which is resolved to where that directory is at
 * the time of the call (at_path).
 */
#include "swap.h"
#include "sysint.h"
#include "syscall.h"
#include "vfs.h"
#include "task.h"
#include "futex.h"
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
 * Fetch a path for an *at call, as an absolute path or one relative to
 * the working directory. Returns 0, or -errno. A path relative to some
 * other open directory is joined onto where that directory is NOW
 * (vfs_dir_path), so a descriptor follows its directory through a
 * rename -- which is what gnulib's fts, and so `grep -r`, rely on.
 */
static int at_path(int dirfd, u32 upath, char *path)
{
    char rel[PATH_MAX];
    struct file *f;
    u32 n;
    int err = fetch_str(rel, upath, PATH_MAX);

    if (err < 0) {
        return err;
    }
    if (!rel[0]) {
        return -ENOENT;
    }
    if (dirfd == AT_FDCWD || rel[0] == '/') {
        strcpy(path, rel);
        return 0;
    }
    f = fd_get(dirfd);
    if (!f) {
        return -EBADF;
    }
    err = vfs_dir_path(f, path, PATH_MAX);
    if (err < 0) {
        return err;             /* -ENOTDIR for a descriptor not a directory */
    }
    n = (u32)strlen(path);
    if (n + 1 + strlen(rel) + 1 > PATH_MAX) {
        return -ENAMETOOLONG;
    }
    if (n > 1) {
        path[n++] = '/';
    }
    strcpy(path + n, rel);
    return 0;
}

/* ---------------------------------------------------------------- */
/* statx                                                             */
/* ---------------------------------------------------------------- */

/*
 * From this system's struct stat to Linux's statx.
 *
 * IF THE FILESYSTEM RECORDED PERMISSIONS, REPORT THEM. ext2 does, and
 * the kernel enforces them (perm_ok in vfs.c) -- so a statx that made
 * up a mode would show `ls -l` one thing while open(2) did another,
 * and every chmod would appear to have no effect.
 *
 * The making-up is kept for a volume that has NO permission bits at
 * all, which is FAT: there the mode is invented the way a Linux FAT
 * mount invents it -- readable by all, writable by the owner if not
 * read-only, executable if it is a program, judged by the same four
 * bytes exec() judges it by so the two never disagree.
 *
 * "No permission bits at all" is the test, rather than asking which
 * filesystem is mounted: a file really can be mode 0, and one that is
 * should read as 0 rather than as 0444.
 */
static void to_statx(const struct stat *st, int is_prog, struct statx *sx)
{
    u32 mode = st->st_mode;
    int have_perms = (mode & 07777) != 0;

    memset(sx, 0, sizeof(*sx));
    if (!have_perms) {
        if (S_ISDIR(mode)) {
            mode = (mode & S_IFMT) | 0755;
        } else if (S_ISREG(mode)) {
            mode = (mode & (S_IFMT | S_IWUSR)) | 0444 | (is_prog ? 0111 : 0);
        } else {
            mode = (mode & S_IFMT) | 0666;
        }
    }

    sx->stx_mask = STATX_BASIC_STATS;
    sx->stx_blksize = 4096;
    sx->stx_nlink = st->st_nlink ? st->st_nlink : 1;
    sx->stx_uid = st->st_uid;
    sx->stx_gid = st->st_gid;
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
    /*
     * AT_SYMLINK_NOFOLLOW: report the LINK rather than its target.
     * `ls -l` passes it, and without it a symlink shows up as whatever
     * it points at -- so a directory full of links looks like a
     * directory full of copies, and a DANGLING link looks like a
     * missing file rather than a broken link.
     */
    err = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(path, &st)
                                        : vfs_stat(path, &st);
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
/* Resource usage                                                    */
/* ---------------------------------------------------------------- */

/* Ticks into a struct rusage: the times, and nothing else, because
 * nothing else is counted. */
static void rusage_of(struct rusage *ru, u32 uticks, u32 sticks)
{
    memset(ru, 0, sizeof(*ru));
    ru->ru_utime.tv_sec = uticks / HZ;
    ru->ru_utime.tv_usec = (uticks % HZ) * (1000000 / HZ);
    ru->ru_stime.tv_sec = sticks / HZ;
    ru->ru_stime.tv_usec = (sticks % HZ) * (1000000 / HZ);
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

    case __NR_lstat: {          /* the LINK, not what it points at */
        struct stat st;

        err = fetch_str(path, a1, sizeof(path));
        if (err < 0) {
            return err;
        }
        err = vfs_lstat(path, &st);
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
        u32 mode = (nr == __NR_chmod) ? a2 : a3;

        if (nr == __NR_chmod) {
            err = fetch_str(path, a1, sizeof(path));
        } else {
            err = at_path((int)a1, a2, path);
        }
        return err < 0 ? err : vfs_setattr(path, ATTR_MODE, mode, 0, 0);
    }

    /*
     * --- priorities: nice values, which set how long a task's turns
     * are (task.c). One user, root, may raise or lower any of them.
     * getpriority returns 20 - nice, Linux's convention, so that no
     * answer looks like an error; the C library turns it back.
     */
    case __NR_getpriority:
    case __NR_setpriority: {
        struct task *m;
        int i, found = 0, best = 20;
        s32 prio = (s32)a3;

        if (a1 > PRIO_USER) {
            return -EINVAL;
        }
        if (prio < -20) {
            prio = -20;
        } else if (prio > 19) {
            prio = 19;
        }
        for (i = 0; (m = task_nth(i)) != 0; i++) {
            int hit;

            if (m->state == TASK_ZOMBIE) {
                continue;
            }
            if (a1 == PRIO_PROCESS) {
                hit = a2 ? m->pid == (int)a2 : m == current;
            } else if (a1 == PRIO_PGRP) {
                hit = m->pgid == (a2 ? (int)a2 : current->pgid);
            } else {
                hit = a2 == 0;          /* everything is root's */
            }
            if (!hit) {
                continue;
            }
            found = 1;
            if (nr == __NR_setpriority) {
                m->nice = (int)prio;
            } else if (m->nice < best) {
                best = m->nice;
            }
        }
        if (!found) {
            return -ESRCH;
        }
        return nr == __NR_getpriority ? 20 - best : 0;
    }

    case __NR_sigaltstack: {
        stack_t ss, old;

        if (a1) {
            err = fetch(&ss, a1, sizeof(ss));
            if (err < 0) {
                return err;
            }
        }
        err = signal_altstack(a1 ? &ss : 0, a2 ? &old : 0);
        if (err == 0 && a2) {
            err = store(a2, &old, sizeof(old));
        }
        return err;
    }

    /* --- sessions --- */
    case __NR_setsid: {
        struct task *m;
        int i;

        /* A group leader cannot start a session, nor anyone whose pid
         * some group already uses (POSIX); a shell's jobs are leaders,
         * which is why setsid(1) forks first. */
        if (current->pgid == current->pid) {
            return -EPERM;
        }
        for (i = 0; (m = task_nth(i)) != 0; i++) {
            if (m->pgid == current->pid && m->state != TASK_ZOMBIE) {
                return -EPERM;
            }
        }
        current->sid = current->pgid = current->pid;
        return current->pid;
    }

    case __NR_getsid: {
        struct task *t = a1 ? task_find((int)a1) : current;

        return t ? t->sid : -ESRCH;
    }

    case __NR_sethostname: {
        extern char hostname[];
        char name[HOST_NAME_MAX + 1];

        if (a2 > HOST_NAME_MAX) {
            return -EINVAL;
        }
        err = fetch(name, a1, a2);
        if (err < 0) {
            return err;
        }
        name[a2] = '\0';
        memcpy(hostname, name, a2 + 1);
        return 0;
    }

    /*
     * File times. Linux/m68k's struct timespec is two 32-bit words; a
     * null `times` is "now" for both, UTIME_NOW and UTIME_OMIT per field,
     * and a null path with a descriptor is futimens().
     */
    case __NR_utimensat: {
        struct timespec ts[2];
        u32 t[2];
        int i;
        struct timeval now;

        clock_get(&now);
        if (a3) {
            err = fetch(ts, a3, sizeof(ts));
            if (err < 0) {
                return err;
            }
            for (i = 0; i < 2; i++) {
                if (ts[i].tv_nsec == UTIME_NOW) {
                    t[i] = (u32)now.tv_sec;
                } else if (ts[i].tv_nsec == UTIME_OMIT) {
                    t[i] = 0xffffffffUL;
                } else if (ts[i].tv_nsec >= 1000000000UL) {
                    return -EINVAL;
                } else {
                    t[i] = (u32)ts[i].tv_sec;
                }
            }
        } else {
            t[0] = t[1] = (u32)now.tv_sec;
        }
        if (a4 & ~(u32)AT_SYMLINK_NOFOLLOW) {
            return -EINVAL;
        }
        if (!a2) {
            return vfs_futime((int)a1, t[1], t[0]);
        }
        err = at_path((int)a1, a2, path);
        return err < 0 ? err : vfs_utime(path, t[1], t[0]);
    }

    case __NR_chroot:
        err = fetch_str(path, a1, sizeof(path));
        return err < 0 ? err : vfs_chroot(path);

    /* An open directory as the working directory. */
    case __NR_fchdir:
        return vfs_fchdir((int)a1);

    /* No permissions to change, as chmod: the descriptor must be open. */
    case __NR_fchmod:
        return vfs_fsetattr((int)a1, ATTR_MODE, a2, 0, 0);

    /*
     * Owners. Everything belongs to root, and there is only root, so
     * "change it to root" -- or to -1, "leave it" -- succeeds and anything
     * else cannot be recorded: EPERM, as a Linux FAT mount answers.
     */
    case __NR_chown:  case __NR_chown32:
    case __NR_lchown: case __NR_lchown32:
    case __NR_fchownat:
    case __NR_fchown: case __NR_fchown32: {
        struct stat st;
        u32 uid = (nr == __NR_fchownat) ? a3 : a2;
        u32 gid = (nr == __NR_fchownat) ? a4 : a3;

        if (nr == __NR_fchown || nr == __NR_fchown32) {
            err = vfs_fstat((int)a1, &st);
        } else {
            if (nr == __NR_fchownat) {
                err = at_path((int)a1, a2, path);
            } else {
                err = fetch_str(path, a1, sizeof(path));
            }
            if (err >= 0) {
                err = vfs_stat(path, &st);
            }
        }
        if (err < 0) {
            return err;
        }
        /* chown's own ids are 16 bits wide; the 32 calls' are 32. */
        if (nr == __NR_chown || nr == __NR_lchown || nr == __NR_fchown) {
            uid = (uid & 0xffff) == 0xffff ? 0xffffffffUL : (uid & 0xffff);
            gid = (gid & 0xffff) == 0xffff ? 0xffffffffUL : (gid & 0xffff);
        }
        /* A descriptor is changed through the descriptor: search
         * permission was settled when it was opened, which is what an
         * fd-based call means. */
        if (nr == __NR_fchown || nr == __NR_fchown32) {
            return vfs_fsetattr((int)a1, ATTR_UID | ATTR_GID, 0, uid, gid);
        }
        return vfs_setattr(path, ATTR_UID | ATTR_GID, 0, uid, gid);
    }

    /*
     * link(2) and linkat(2). These answered -EPERM from when the
     * filesystem was FAT, which has no link count and no way to have
     * two names for one file; ext2 has both, and vfs_link does it.
     *
     * linkat's flags are ignored except AT_SYMLINK_FOLLOW, which is
     * about symbolic links and so means nothing until there are any.
     */
    case __NR_link:
    case __NR_linkat: {
        char to[PATH_MAX];

        if (nr == __NR_link) {
            err = fetch_str(path, a1, sizeof(path));
            if (err >= 0) {
                err = fetch_str(to, a2, sizeof(to));
            }
        } else {
            err = at_path((int)a1, a2, path);
            if (err >= 0) {
                err = at_path((int)a3, a4, to);
            }
        }
        return err < 0 ? err : vfs_link(path, to);
    }

    /* Device nodes and FIFOs still have nowhere to live: making one
     * needs a node type on disk that nothing here writes yet. */
    case __NR_mknod:
    case __NR_mknodat:
        return -EPERM;

    case __NR_getrusage: {
        struct rusage ru;

        if ((s32)a1 == RUSAGE_SELF) {
            rusage_of(&ru, current->utime, current->stime);
        } else if ((s32)a1 == RUSAGE_CHILDREN) {
            rusage_of(&ru, current->cutime, current->cstime);
        } else {
            return -EINVAL;
        }
        return store(a2, &ru, sizeof(ru));
    }

    case __NR_readlink:
    case __NR_readlinkat: {
        char target[PATH_MAX];
        u32 want = (nr == __NR_readlink) ? a3 : a4;
        u32 n;

        if (nr == __NR_readlink) {
            err = fetch_str(path, a1, sizeof(path));
        } else {
            err = at_path((int)a1, a2, path);
        }
        if (err < 0) {
            return err;
        }
        err = vfs_readlink(path, target, sizeof(target));
        if (err < 0) {
            return err;
        }
        /*
         * readlink does NOT null-terminate, and returns how many bytes
         * it wrote. A caller that expects a terminator adds its own;
         * one that gets a terminator it did not ask for gets a path
         * one byte too long. Truncation is silent, as Linux's is.
         */
        n = (u32)strlen(target);
        if (n > want) {
            n = want;
        }
        err = store((nr == __NR_readlink) ? a2 : a3, target, n);
        return err < 0 ? err : (int)n;
    }

    case __NR_swapon:
    case __NR_swapoff:
        err = fetch_str(path, a1, sizeof(path));
        if (err < 0) {
            return err;
        }
        if (nr == __NR_swapon) {
            err = swap_on(path);
            return err < 0 ? err : 0;
        }
        if (!swap_matches(path)) {
            return -EINVAL;             /* Linux's answer: not a swap file */
        }
        err = vm_swapoff();
        if (err < 0) {
            return err;
        }
        swap_release();
        return 0;

    case __NR_flock:
        return vfs_flock((int)a1, (int)a2);

    case __NR_ftruncate:
        if ((s32)a2 < 0) {
            return -EINVAL;
        }
        return vfs_ftruncate((int)a1, a2);

    case __NR_truncate: {
        int fd;

        if ((s32)a2 < 0) {
            return -EINVAL;
        }
        err = fetch_str(path, a1, sizeof(path));
        if (err < 0) {
            return err;
        }
        fd = fd_open(path, O_WRONLY);
        if (fd < 0) {
            return fd;
        }
        err = vfs_ftruncate(fd, a2);
        fd_close(fd);
        return err;
    }

    case __NR_symlink:
    case __NR_symlinkat: {
        char target[PATH_MAX];

        /* The TARGET is a string, not a path to resolve -- it is
         * stored as given and may name nothing at all. Only the link's
         * own location is a path. */
        err = fetch_str(target, a1, sizeof(target));
        if (err < 0) {
            return err;
        }
        if (nr == __NR_symlink) {
            err = fetch_str(path, a2, sizeof(path));
        } else {
            err = at_path((int)a2, a3, path);
        }
        return err < 0 ? err : vfs_symlink(target, path);
    }

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

        {   /* the process's, not the thread's: CLONE_FS again */
            struct task *t;
            int i;

            for (i = 0; (t = task_nth(i)) != 0; i++) {
                if (t->tgid == current->tgid) {
                    t->umask = a1 & 0777;
                }
            }
        }
        return (s32)old;
    }

    /*
     * --- identity ---
     *
     * Real, effective and saved, kept per task. They used to be a
     * constant 0: one user, root, and setuid(0) the only call that
     * succeeded. See struct task for what having them properly does
     * and does not buy -- identity is real, ENFORCEMENT against files
     * is not, because FAT cannot record an owner.
     */
    case __NR_getuid:  case __NR_getuid32:
        return (s32)current->uid;
    case __NR_geteuid: case __NR_geteuid32:
        return (s32)current->euid;
    case __NR_getgid:  case __NR_getgid32:
        return (s32)current->gid;
    case __NR_getegid: case __NR_getegid32:
        return (s32)current->egid;

    /*
     * setuid(): POSIX's rule exactly. Root may become anybody, and
     * doing so sets all three so the change cannot be undone. Anybody
     * else may only move between the identities they already hold --
     * their real and their saved -- which is what lets a program drop
     * a privilege and take it back.
     */
    case __NR_setuid:  case __NR_setuid32: {
        u32 want = a1;
        struct task *t;
        int i;

        /*
         * WAS IT ROOT? Decided BEFORE the loop, and this is not a
         * tidying-up: the loop assigns t->euid, and when t is current
         * that overwrites current->euid -- so a test of
         * `current->euid == 0` inside the loop is false by the time it
         * is reached, on the very first iteration.
         *
         * The effect was that root's setuid(N) moved only the
         * effective id and left the real and saved ones at 0, so the
         * process could setuid(0) straight back. su reported "could
         * not drop privilege", which is the good outcome; the bad one
         * is a program that does not check and believes it dropped.
         */
        int was_root = (current->euid == 0);

        if (current->euid != 0 &&
            want != current->uid && want != current->suid) {
            return -EPERM;
        }
        /*
         * EVERY THREAD OF THE PROCESS, not just this one. Credentials
         * belong to the process; a thread that kept the old uid while
         * its siblings changed would be a hole rather than a feature.
         * The same reason cwd is written through in vfs_cwd_set.
         */
        for (i = 0; (t = task_nth(i)) != 0; i++) {
            if (t->tgid != current->tgid) {
                continue;
            }
            t->euid = want;
            if (was_root) {
                t->uid = t->suid = want;
            }
        }
        return 0;
    }

    case __NR_setgid:  case __NR_setgid32: {
        u32 want = a1;
        struct task *t;
        /* Hoisted for the same reason as setuid's above. It is not
         * actually unsafe here -- this loop writes egid and reads
         * euid, so it does not overwrite what it tests -- but the two
         * cases are the same shape, and one written the dangerous way
         * beside one written the safe way invites the wrong one being
         * copied. */
        int was_root = (current->euid == 0);
        int i;

        if (!was_root &&
            want != current->gid && want != current->sgid) {
            return -EPERM;
        }
        for (i = 0; (t = task_nth(i)) != 0; i++) {
            if (t->tgid != current->tgid) {
                continue;
            }
            t->egid = want;
            if (was_root) {
                t->gid = t->sgid = want;
            }
        }
        return 0;
    }

    /*
     * setreuid(ruid, euid): -1 for either means "leave it". Root may
     * set both; anybody else may only swap between the ones they
     * hold. The saved id follows the effective one whenever the real
     * one changes or the effective is set to something other than the
     * real, which is what POSIX says and what makes a swap
     * reversible.
     */
    case __NR_setreuid: case __NR_setreuid32:
    case __NR_setregid: case __NR_setregid32: {
        int is_uid = (nr == __NR_setreuid || nr == __NR_setreuid32);
        s32 r = (s32)a1, e = (s32)a2;
        u32 cur_r = is_uid ? current->uid : current->gid;
        u32 cur_e = is_uid ? current->euid : current->egid;
        u32 cur_s = is_uid ? current->suid : current->sgid;
        u32 new_r = r == -1 ? cur_r : (u32)r;
        u32 new_e = e == -1 ? cur_e : (u32)e;
        struct task *t;
        int i;

        if (current->euid != 0) {
            if (r != -1 && new_r != cur_r && new_r != cur_e) {
                return -EPERM;
            }
            if (e != -1 && new_e != cur_r && new_e != cur_e &&
                new_e != cur_s) {
                return -EPERM;
            }
        }
        for (i = 0; (t = task_nth(i)) != 0; i++) {
            if (t->tgid != current->tgid) {
                continue;
            }
            if (is_uid) {
                t->uid = new_r;
                t->euid = new_e;
                if (r != -1 || new_e != new_r) {
                    t->suid = new_e;
                }
            } else {
                t->gid = new_r;
                t->egid = new_e;
                if (r != -1 || new_e != new_r) {
                    t->sgid = new_e;
                }
            }
        }
        return 0;
    }

    /*
     * setresuid(r, e, s) / setresgid: all three at once, -1 for any
     * that is to stay. Root may set anything; anybody else may only
     * shuffle the three they already hold, which is exactly enough to
     * drop a privilege and pick it up again and no more.
     */
    case __NR_setresuid: case __NR_setresuid32:
    case __NR_setresgid: case __NR_setresgid32: {
        int is_uid = (nr == __NR_setresuid || nr == __NR_setresuid32);
        s32 r = (s32)a1, e = (s32)a2, sv = (s32)a3;
        u32 cur_r = is_uid ? current->uid  : current->gid;
        u32 cur_e = is_uid ? current->euid : current->egid;
        u32 cur_s = is_uid ? current->suid : current->sgid;
        u32 new_r = r  == -1 ? cur_r : (u32)r;
        u32 new_e = e  == -1 ? cur_e : (u32)e;
        u32 new_s = sv == -1 ? cur_s : (u32)sv;
        struct task *t;
        int i;

        if (current->euid != 0) {
            u32 want[3];
            int k;

            want[0] = new_r;
            want[1] = new_e;
            want[2] = new_s;
            for (k = 0; k < 3; k++) {
                if (want[k] != cur_r && want[k] != cur_e &&
                    want[k] != cur_s) {
                    return -EPERM;
                }
            }
        }
        for (i = 0; (t = task_nth(i)) != 0; i++) {
            if (t->tgid != current->tgid) {
                continue;
            }
            if (is_uid) {
                t->uid = new_r; t->euid = new_e; t->suid = new_s;
            } else {
                t->gid = new_r; t->egid = new_e; t->sgid = new_s;
            }
        }
        return 0;
    }

    case __NR_getresuid: case __NR_getresuid32:
    case __NR_getresgid: case __NR_getresgid32: {
        int is_uid = (nr == __NR_getresuid || nr == __NR_getresuid32);
        u32 v[3];
        int e1, e2, e3;

        v[0] = is_uid ? current->uid  : current->gid;
        v[1] = is_uid ? current->euid : current->egid;
        v[2] = is_uid ? current->suid : current->sgid;
        e1 = store(a1, &v[0], sizeof(v[0]));
        e2 = store(a2, &v[1], sizeof(v[1]));
        e3 = store(a3, &v[2], sizeof(v[2]));
        if (e1 < 0) { return e1; }
        if (e2 < 0) { return e2; }
        if (e3 < 0) { return e3; }
        return 0;
    }

    /*
     * The supplementary groups. These used to answer "none" and refuse
     * anything else, which was honest while nothing could decide a
     * group permission. perm_ok() in vfs.c decides one now, and login
     * sets the list from /etc/group, so they are real.
     *
     * getgroups(0, ...) is "how many are there", and must not touch
     * the buffer -- that is how a caller sizes one.
     */
    case __NR_getgroups:
    case __NR_getgroups32: {
        int n = current ? current->ngroups : 0;
        int want = (int)a1;
        u32 buf[NGROUPS_MAX];
        int i;

        if (want == 0) {
            return n;
        }
        if (want < n) {
            return -EINVAL;
        }
        for (i = 0; i < n; i++) {
            buf[i] = current->groups[i];
        }
        if (n > 0) {
            err = store(a2, buf, (u32)n * sizeof(u32));
            if (err < 0) {
                return err;
            }
        }
        return n;
    }

    case __NR_setgroups:
    case __NR_setgroups32: {
        int n = (int)a1;
        u32 buf[NGROUPS_MAX];
        int i;

        if (!current) {
            return -EPERM;
        }
        /* ONLY ROOT. Otherwise any user could join any group simply by
         * saying so, and every group permission on the disk would mean
         * nothing at all. */
        if (current->euid != 0) {
            return -EPERM;
        }
        if (n < 0 || n > NGROUPS_MAX) {
            return -EINVAL;
        }
        if (n > 0) {
            err = fetch(buf, a2, (u32)n * sizeof(u32));
            if (err < 0) {
                return err;
            }
        }
        for (i = 0; i < n; i++) {
            current->groups[i] = buf[i];
        }
        current->ngroups = n;
        return 0;
    }

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

            /* The reaped child's times; a stopped child has used time
             * but not finished using it, so reports none, as Linux's
             * wait4 does for a child it does not reap. */
            rusage_of(&ru, current->waited_utime, current->waited_stime);
            current->waited_utime = current->waited_stime = 0;
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
     */
    case __NR_vfork: {
        struct task *t = task_fork(regs);

        return t ? t->pid : (task_count() >= TASK_MAX ? -EAGAIN : -ENOMEM);
    }

    /*
     * clone: fork, or a thread. m68k passes the arguments in the order
     * Linux/m68k's own sys_clone reads them -- flags, the child's stack,
     * the parent's tid word, the child's tid word, then the TLS pointer.
     * task_clone says which combinations are implemented and refuses the
     * rest; see the note there.
     */
    case __NR_clone: {
        struct task *t;
        int err = 0;

        t = task_clone(regs, a1, a2, a3, a4, a5, &err);
        return t ? t->pid : err;
    }

    /* The THREAD's id, where getpid() gives the process's. */
    case __NR_gettid:
        return current->pid;

    /*
     * The word to clear and wake when this thread ends, which is what a
     * joiner sleeps on. Returns the caller's own tid, as Linux does.
     */
    case __NR_set_tid_address:
        current->clear_child_tid = a1;
        return current->pid;

    /* Sleep unless this word has changed, and wake whoever is asleep on
     * one. Everything a thread library waits on is built out of these. */
    case __NR_futex:
        return futex_call(a1, (int)a2, a3, a4, a5, a6);

    /*
     * exit_group(): the whole process, threads and all. This is what a
     * C library's exit() calls, so a threaded program that returns from
     * main takes its threads with it rather than leaving them running
     * with nothing to run for.
     */
    case __NR_exit_group:
        task_group_kill(current);
        task_exit((int)a1 & 0xff);
        return 0;               /* not reached */

    /* A signal to ONE thread, rather than to the process. */
    case __NR_tkill:
    case __NR_tgkill: {
        int tid = (nr == __NR_tkill) ? (int)a1 : (int)a2;
        int sig = (nr == __NR_tkill) ? (int)a2 : (int)a3;
        struct task *t = task_find(tid);

        if (!t || !t->as || t->state == TASK_ZOMBIE) {
            return -ESRCH;
        }
        if (nr == __NR_tgkill && t->tgid != (int)a1) {
            return -ESRCH;
        }
        return sig ? signal_send(t, sig) : 0;
    }

    /*
     * Robust futexes -- a list the kernel walks to release the locks a
     * thread died holding. Accepted and not kept: nothing here dies
     * holding a lock without the process ending too, and pretending to
     * support it would be worse than saying so. Returns 0 because
     * glibc-shaped startup code calls it once and carries on.
     */
    case __NR_set_robust_list:
        return 0;

    /* --- time, limits, randomness --- */
    case __NR_clock_gettime:
        return do_clock_gettime((int)a1, a2);

    case __NR_clock_getres: {
        /* Every clock here counts in ticks, so a tick is the answer. */
        struct timespec ts;

        if ((int)a1 != CLOCK_REALTIME && (int)a1 != CLOCK_MONOTONIC &&
            (int)a1 != CLOCK_PROCESS_CPUTIME_ID &&
            (int)a1 != CLOCK_THREAD_CPUTIME_ID) {
            return -EINVAL;
        }
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000000UL / HZ;
        return a2 ? store(a2, &ts, sizeof(ts)) : 0;
    }

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

    /*
     * Waits until the pool is ready, as Linux's does; GRND_NONBLOCK makes
     * that EAGAIN instead. GRND_RANDOM is the same thing, as it has been
     * on Linux since 5.6. Through a kernel buffer, 256 bytes at a time.
     */
    case __NR_getrandom: {
        u8 buf[256];
        u32 done = 0;

        if (a3 & ~(u32)(GRND_NONBLOCK | GRND_RANDOM)) {
            return -EINVAL;
        }
        err = random_wait((a3 & GRND_NONBLOCK) != 0);
        if (err < 0) {
            return err;
        }
        while (done < a2) {
            u32 n = a2 - done < sizeof(buf) ? a2 - done : sizeof(buf);

            random_get(buf, n);
            err = store(a1 + done, buf, n);
            if (err < 0) {
                memset(buf, 0, sizeof(buf));
                return done ? (s32)done : err;
            }
            done += n;
        }
        memset(buf, 0, sizeof(buf));
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
