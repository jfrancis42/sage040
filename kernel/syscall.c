/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * syscall.c - the system call table and the wrappers around it.
 *
 * Everything above this file -- the shell today, user programs later --
 * reaches the kernel only through `trap #0`. Nothing calls the VFS or a
 * driver directly, which is what makes the boundary real rather than
 * decorative: when programs start running unprivileged, the code above
 * does not change at all, because it was never on this side of the gate
 * to begin with.
 *
 * Pointer arguments are still taken at face value. That is correct while
 * every caller shares the kernel's address space, and this is the
 * function that will have to validate and copy them when that stops
 * being true. It is marked here rather than in a plan somewhere because
 * this is where the work goes.
 */
#include "syscall.h"
#include "sysint.h"
#include "vfs.h"
#include "exec.h"
#include "task.h"
#include "signal.h"
#include "wait.h"
#include "tty.h"
#include "timer.h"
#include "dev.h"
#include "console.h"
#include "uaccess.h"
#include "pmm.h"
#include "vm.h"
#include "cache.h"
#include "mmap.h"
#include "drivers/drivers.h"
#include "textcache.h"
#include "swap.h"
#include "poll.h"
#include "pipe.h"
#include "ptregs.h"
#include "net.h"
#include "tcp.h"
#include "errno.h"
#include "string.h"


/*
 * Nothing ever wakes this. It exists so that nanosleep() has somewhere
 * to wait: sleep_on_timeout() needs a queue, and the only two ways out
 * of this one are the timeout expiring and a signal arriving.
 */
static struct waitq sleep_waitq;

/* ---------------------------------------------------------------- */
/* Getting at the caller's memory                                    */
/*                                                                    */
/* A program's pointers are addresses in ITS address space and mean   */
/* nothing in the kernel's, so every one of them has to be fetched    */
/* rather than dereferenced.                                          */
/*                                                                    */
/* The shell is the exception, and it is a transitional one. It runs  */
/* inside the kernel and reaches the system through this same gate,   */
/* so its pointers ARE kernel pointers. uaccess_current() is non-null */
/* only while a user program is running, which distinguishes the two  */
/* exactly. When the shell moves out of the kernel -- which is the    */
/* whole direction of travel -- every branch below collapses to its   */
/* user side and these helpers become copy_from_user and friends.     */
/* ---------------------------------------------------------------- */

int from_program(void)
{
    return uaccess_current() != 0;
}

int fetch(void *dst, u32 p, u32 len)
{
    if (!p) {
        return -EFAULT;
    }
    if (!from_program()) {
        memcpy(dst, (const void *)p, len);
        return 0;
    }
    return copy_from_user(dst, p, len);
}

int store(u32 p, const void *src, u32 len)
{
    if (!p) {
        return -EFAULT;
    }
    if (!from_program()) {
        memcpy((void *)p, src, len);
        return 0;
    }
    return copy_to_user(p, src, len);
}

/* Returns the length, or -errno. */
int fetch_str(char *dst, u32 p, u32 max)
{
    if (!p) {
        return -EFAULT;
    }
    if (!from_program()) {
        const char *src = (const char *)p;
        u32 i;

        for (i = 0; i < max; i++) {
            dst[i] = src[i];
            if (!src[i]) {
                return (int)i;
            }
        }
        return -ENAMETOOLONG;
    }
    return strncpy_from_user(dst, p, max);
}

/*
 * read() and write() move bulk data, and bouncing it through a kernel
 * buffer would copy every byte twice for no reason. Instead the user
 * buffer is walked a page at a time -- which is as far as it is
 * guaranteed to be contiguous in physical memory -- and the filesystem
 * reads or writes straight into it.
 */
static s32 rw_user(int fd, u32 ubuf, u32 len, int writing)
{
    s32 total = 0;

    if (!from_program()) {
        return writing ? fd_write(fd, (const void *)ubuf, len)
                       : fd_read(fd, (void *)ubuf, len);
    }

    while (len > 0) {
        u32 n = len;
        void *k = uaccess_chunk(ubuf, &n, !writing);
        s32 got;

        if (!k) {
            return total > 0 ? total : -EFAULT;
        }
        /*
         * PINNED for the length of the call: a read from a pipe or a
         * terminal can sleep with this physical address in hand, and
         * another task's fault could otherwise evict the page and give
         * it away meanwhile. Reclaim never takes a page with two
         * holders, and this is the second.
         */
        pmm_ref(PAGE_ALIGN_DOWN((u32)k));
        got = writing ? fd_write(fd, k, n) : fd_read(fd, k, n);
        pmm_free(PAGE_ALIGN_DOWN((u32)k));
        if (got < 0) {
            return total > 0 ? total : got;
        }
        total += got;
        if ((u32)got < n) {
            break;              /* short: end of file, or a full device */
        }
        ubuf += n;
        len -= n;
        /*
         * A read that has something must not then wait for more. The
         * buffer is handed over a page at a time, and a read spanning
         * two pages used to ask the file a second time -- which, for a
         * pipe or a terminal that had given all it had, meant sleeping
         * with data already in hand.
         */
        if (!writing && len > 0 && !(poll_fd(fd) & POLLIN)) {
            break;
        }
    }
    return total;
}

/* ---------------------------------------------------------------- */
/* The implementations                                               */
/* ---------------------------------------------------------------- */

/* The seconds of the one clock -- the same one gettimeofday() reads. */
static time_t clock_now(void)
{
    struct timeval tv;

    clock_get(&tv);
    return (time_t)tv.tv_sec;
}

/* The host name: sethostname() sets it, uname() reports it. */
char hostname[HOST_NAME_MAX + 1] = "sage040";

static int do_uname(struct utsname *u)
{
    if (!u) {
        return -EINVAL;
    }
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, KERNEL_NAME);
    strcpy(u->release, kernel_version);
    strcpy(u->machine, "m68040");
    strcpy(u->version, kernel_build);
    strcpy(u->nodename, hostname);
    return 0;
}

static int do_stime(const time_t *t)
{
    struct timeval tv;

    if (!t) {
        return -EINVAL;
    }
    tv.tv_sec = (s32)*t;
    tv.tv_usec = 0;
    return clock_set(&tv);
}

/* --- interval timers ---------------------------------------------------- */

/* A timeval as ticks, rounded UP: a timer shorter than a tick is a tick,
 * not nothing. -1 if it is not a valid time. */
static s32 tv_to_ticks(const struct timeval *tv)
{
    u32 us_per_tick = 1000000 / HZ;

    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000 ||
        tv->tv_sec > 0x7fffffff / HZ - 1) {
        return -1;
    }
    return tv->tv_sec * HZ +
           (s32)(((u32)tv->tv_usec + us_per_tick - 1) / us_per_tick);
}

static void ticks_to_tv(u32 ticks, struct timeval *tv)
{
    tv->tv_sec = (s32)(ticks / HZ);
    tv->tv_usec = (s32)((ticks % HZ) * (1000000 / HZ));
}

static int itimer_get(int which, struct itimerval *out)
{
    struct task *t = current;
    u32 now = timer_jiffies(), left = 0, interval;

    switch (which) {
    case ITIMER_REAL:
        if (t->it_real_at && (s32)(t->it_real_at - now) > 0) {
            left = t->it_real_at - now;
        } else if (t->it_real_at) {
            left = 1;               /* due this tick */
        }
        interval = t->it_real_interval;
        break;
    case ITIMER_VIRTUAL:
        left = t->it_virt;
        interval = t->it_virt_interval;
        break;
    case ITIMER_PROF:
        left = t->it_prof;
        interval = t->it_prof_interval;
        break;
    default:
        return -EINVAL;
    }
    ticks_to_tv(left, &out->it_value);
    ticks_to_tv(interval, &out->it_interval);
    return 0;
}

static int itimer_set(int which, const struct itimerval *in)
{
    struct task *t = current;
    s32 value = tv_to_ticks(&in->it_value);
    s32 interval = tv_to_ticks(&in->it_interval);
    u16 sr;

    if (value < 0 || interval < 0) {
        return -EINVAL;
    }
    /* Masked: the tick reads and writes these. */
    sr = irq_save();
    switch (which) {
    case ITIMER_REAL:
        t->it_real_at = value ? timer_jiffies() + (u32)value : 0;
        if (value && t->it_real_at == 0) {
            t->it_real_at = 1;      /* 0 means off; do not land on it */
        }
        t->it_real_interval = (u32)interval;
        break;
    case ITIMER_VIRTUAL:
        t->it_virt = (u32)value;
        t->it_virt_interval = (u32)interval;
        break;
    case ITIMER_PROF:
        t->it_prof = (u32)value;
        t->it_prof_interval = (u32)interval;
        break;
    default:
        irq_restore(sr);
        return -EINVAL;
    }
    irq_restore(sr);
    return 0;
}

static void do_reboot(int cmd)
{
    vfs_sync();
    /* Unmounting is what marks the volume clean; a machine stopped
     * without it is checked at the next boot. */
    vfs_shutdown();

    /*
     * Three things are being asked for and they are not the same.
     *
     * RB_HALT_SYSTEM stops the CPU and leaves the machine sitting
     * there, which is what `halt` has always done here: the emulator
     * keeps running and a harness watching the serial output is what
     * notices.
     *
     * RB_AUTOBOOT restarts it. This board can: the keyboard controller
     * pulls the processor's reset line, which is how every PC since
     * 1984 has rebooted itself.
     *
     * RB_POWER_OFF asks for the machine to go away, which needs
     * something that can cut the supply. Nothing on this board can, so
     * the reset is used instead and the machine is told so -- under the
     * emulator, started with -no-reboot, a reset ends the process, so
     * "off" and "reset" are the same event and `shutdown` does what it
     * says. On real hardware they are not, and somebody reading the
     * console deserves to know which one they got. Saying so beats
     * pretending, because a `shutdown` that silently restarted the
     * machine would be worse than one that admits what it can do.
     */
    /*
     * Note for anyone editing this: dev_reset() and dev_poweroff()
     * return ONLY when they did not work. Success is the machine
     * ceasing to exist, which no return value can report -- so there is
     * no "== 0" case to write, and a `for (;;) halt()` after a
     * successful one would be unreachable.
     */
    switch ((u32)cmd) {
    case RB_HALT_SYSTEM:
        break;

    case RB_AUTOBOOT:
        dev_reset();
        kputln("reboot: nothing on this board can reset it; halting");
        break;

    case RB_POWER_OFF:
        dev_poweroff();
        /* Printed before the attempt, because a reset that works never
         * comes back to print anything. */
        kputln("shutdown: no power control on this board; resetting");
        dev_reset();
        kputln("shutdown: nothing on this board can stop it; halting");
        break;

    default:
        break;
    }
    halt();
}

/* --- ioctl --------------------------------------------------------- */

/*
 * ioctl is the awkward one: what its third argument means depends
 * entirely on the second. Some requests pass a value, some a pointer to
 * a structure going in, some a pointer to one coming out, and one passes
 * a structure that goes both ways.
 *
 * A table, rather than a case for each, because the alternative is the
 * same six lines written a dozen times and a new device driver quietly
 * forgetting them. A request that is not in the table takes its argument
 * by value -- which is right for FBIO_CLEAR and FBIO_DOUBLE, and is also
 * what makes an unknown request reach the driver and come back -ENOTTY
 * instead of being rejected here.
 */
#define IO_IN   1
#define IO_OUT  2

static const struct {
    u32 request;
    u16 size;
    u8  dir;
} ioctl_args[] = {
    { FIONREAD,     sizeof(u32),                  IO_OUT },
    { FIONBIO,      sizeof(int),                  IO_IN  },
    { TCGETS,       sizeof(struct termios),       IO_OUT },
    { TCSETS,       sizeof(struct termios),       IO_IN  },
    { TCSETSW,      sizeof(struct termios),       IO_IN  },
    { TCSETSF,      sizeof(struct termios),       IO_IN  },
    { TCGETS2,      sizeof(struct termios2),      IO_OUT },
    { TCSETS2,      sizeof(struct termios2),      IO_IN  },
    { TCSETSW2,     sizeof(struct termios2),      IO_IN  },
    { TCSETSF2,     sizeof(struct termios2),      IO_IN  },
    { TIOCGCONS,    sizeof(struct console_info),  IO_IN | IO_OUT },
    { TIOCSCONS,    sizeof(struct console_set),   IO_IN  },
    { TIOCGPGRP,    sizeof(int),                  IO_OUT },
    { TIOCSPGRP,    sizeof(int),                  IO_IN  },
    { TIOCGPTN,     sizeof(int),                  IO_OUT },
    { TIOCSPTLCK,   sizeof(int),                  IO_IN  },
    { TIOCGWINSZ,   sizeof(struct winsize),       IO_OUT },
    { TIOCSWINSZ,   sizeof(struct winsize),       IO_IN  },
    { FBIO_GETINFO, sizeof(struct fb_info),       IO_OUT },
    { FBIO_SETMODE, sizeof(struct fb_mode),       IO_IN  },
    { FBIO_POINT,   sizeof(struct fb_point),      IO_IN  },
    { FBIO_LINE,    sizeof(struct fb_line),       IO_IN  },
    { FBIO_RECT,    sizeof(struct fb_rect),       IO_IN  },
    { FBIO_COPY,    sizeof(struct fb_copy),       IO_IN  },
    { FBIO_PALETTE, sizeof(struct fb_palette),    IO_IN  }
};

#define IOCTL_BUF_MAX  64

static int do_ioctl(int fd, u32 request, u32 arg)
{
    u8 buf[IOCTL_BUF_MAX];
    unsigned i;
    int err;

    if (!from_program()) {
        return fd_ioctl(fd, request, arg);
    }

    for (i = 0; i < sizeof(ioctl_args) / sizeof(ioctl_args[0]); i++) {
        if (ioctl_args[i].request != request) {
            continue;
        }
        if (ioctl_args[i].size > IOCTL_BUF_MAX) {
            return -EINVAL;     /* the table outgrew the buffer */
        }
        memset(buf, 0, sizeof(buf));

        if (ioctl_args[i].dir & IO_IN) {
            err = fetch(buf, arg, ioctl_args[i].size);
            if (err < 0) {
                return err;
            }
        }
        err = fd_ioctl(fd, request, (u32)buf);
        if (err < 0) {
            return err;
        }
        if (ioctl_args[i].dir & IO_OUT) {
            int e2 = store(arg, buf, ioctl_args[i].size);

            if (e2 < 0) {
                return e2;
            }
        }
        return err;
    }

    /* By value. */
    return fd_ioctl(fd, request, arg);
}

/* --- spawn --------------------------------------------------------- */

/*
 * Copying in a whole argument vector.
 *
 * Two levels of user pointer: the array itself, and every string in it.
 * Both have to be fetched, and the strings have to end up in kernel
 * memory because exec_spawn will copy them into a DIFFERENT address
 * space -- the new program's -- by which time the old one may not even
 * be mapped.
 */
/*
 * THE ARGUMENTS OF A NEW PROGRAM, copied into the kernel.
 *
 * They have to be: the new program's address space is a different one,
 * and for execve the old one is destroyed before the new program runs.
 * They used to be copied into fixed arrays on the kernel stack -- eight
 * arguments of 64 bytes -- which is both too small for `sh -c "some
 * long command"` and too big to grow on an 8 KB stack. So they go into
 * ARG_PAGES of their own, pointer arrays first and the strings packed
 * after them, and that block is given back once the new image is built.
 */
#define ARG_PAGES       8                       /* 32 KB, all told */
#define ARG_BYTES       (ARG_PAGES * PAGE_SIZE)

struct argblock {
    u32   pa;                   /* the pages, 0 if none                */
    int   argc;
    char **argv;                /* EXEC_MAX_ARGS + 1 slots             */
    char **envp;                /* EXEC_MAX_ENV + 1 slots              */
};

static void args_free(struct argblock *b)
{
    if (b->pa) {
        pmm_free_pages(b->pa, ARG_PAGES);
        b->pa = 0;
    }
}

/*
 * `argc` of -1 means "count them, up to the null", which is execve's
 * way; spawn says how many. `uenvp` of 0 is an empty environment.
 */
static int args_fetch(struct argblock *b, int argc, u32 uargv, u32 uenvp)
{
    char *strings, *end;
    u32 p;
    int i, n, err;

    b->pa = pmm_alloc_pages(ARG_PAGES);
    if (!b->pa) {
        return -ENOMEM;
    }
    b->argv = (char **)b->pa;
    b->envp = b->argv + EXEC_MAX_ARGS + 1;
    strings = (char *)(b->envp + EXEC_MAX_ENV + 1);
    end = (char *)b->pa + ARG_BYTES;

    for (i = 0; argc < 0 || i < argc; i++) {
        if (i == EXEC_MAX_ARGS) {
            err = -E2BIG;
            goto fail;
        }
        err = fetch(&p, uargv + (u32)i * 4, sizeof(p));
        if (err < 0) {
            goto fail;
        }
        if (!p) {
            if (argc < 0) {
                break;          /* execve: the end of the list */
            }
            err = -EFAULT;
            goto fail;
        }
        n = fetch_str(strings, p, (u32)(end - strings));
        if (n < 0) {
            err = n == -ENAMETOOLONG ? -E2BIG : n;
            goto fail;
        }
        b->argv[i] = strings;
        strings += n + 1;
    }
    b->argc = i;
    b->argv[i] = 0;

    for (i = 0; uenvp; i++) {
        if (i == EXEC_MAX_ENV) {
            err = -E2BIG;
            goto fail;
        }
        err = fetch(&p, uenvp + (u32)i * 4, sizeof(p));
        if (err < 0) {
            goto fail;
        }
        if (!p) {
            break;
        }
        n = fetch_str(strings, p, (u32)(end - strings));
        if (n < 0) {
            err = n == -ENAMETOOLONG ? -E2BIG : n;
            goto fail;
        }
        b->envp[i] = strings;
        strings += n + 1;
    }
    b->envp[i] = 0;
    return 0;

fail:
    args_free(b);
    return err;
}

/*
 * noinline, like every system call here with big buffers: inlined into
 * do_syscall, their buffers become part of do_syscall's frame, and then
 * EVERY call pays for them in kernel stack -- 1,840 bytes of it, when
 * this was measured -- not just the calls that use them.
 */
static __attribute__((noinline))
int do_spawn(u32 upath, int argc, u32 uargv, u32 uenvp)
{
    char path[PATH_MAX];
    struct argblock b;
    int err;

    if (argc < 0) {
        return -EINVAL;
    }
    err = fetch_str(path, upath, sizeof(path));
    if (err < 0) {
        return err;
    }
    err = args_fetch(&b, argc, uargv, uenvp);
    if (err < 0) {
        return err;
    }
    err = exec_spawn(path, b.argc, b.argv, b.envp);
    args_free(&b);
    return err;
}

static __attribute__((noinline))
int do_execve(u32 upath, u32 uargv, u32 uenvp, struct pt_regs *regs)
{
    char path[PATH_MAX];
    struct argblock b;
    int err;

    err = fetch_str(path, upath, sizeof(path));
    if (err < 0) {
        return err;
    }
    err = args_fetch(&b, -1, uargv, uenvp);
    if (err < 0) {
        return err;
    }
    err = exec_replace(path, b.argc, b.argv, b.envp, regs);
    args_free(&b);
    return err;
}

/* --- waiting on descriptors ------------------------------------------ */

static __attribute__((noinline))
int do_poll(u32 ufds, u32 n, s32 timeout_ms)
{
    struct pollfd local[POLL_MAX];
    int r, err;

    if (n > POLL_MAX) {
        return -EINVAL;
    }
    if (n) {
        err = fetch(local, ufds, n * sizeof(struct pollfd));
        if (err < 0) {
            return err;
        }
    }
    r = poll_files(local, n, timeout_ms, 0);
    if (r < 0) {
        return r;
    }
    if (n) {
        err = store(ufds, local, n * sizeof(struct pollfd));
        if (err < 0) {
            return err;
        }
    }
    return r;
}

static __attribute__((noinline))
int do_select(u32 nfds, u32 uin, u32 uout, u32 uex, u32 utv)
{
    u32 in[FD_SETSIZE / 32], out[FD_SETSIZE / 32], ex[FD_SETSIZE / 32];
    u32 bytes = ((nfds + 31) / 32) * 4;
    s32 timeout_ms = -1, left = 0;
    struct timeval tv;
    int r, err;

    if (nfds > FD_SETSIZE) {
        return -EINVAL;
    }
    if ((uin && (err = fetch(in, uin, bytes)) < 0) ||
        (uout && (err = fetch(out, uout, bytes)) < 0) ||
        (uex && (err = fetch(ex, uex, bytes)) < 0)) {
        return err;
    }
    if (utv) {
        err = fetch(&tv, utv, sizeof(tv));
        if (err < 0) {
            return err;
        }
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
            return -EINVAL;
        }
        /* Rounded up to the millisecond, so a short wait is not none. */
        timeout_ms = tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
    }

    r = poll_select(nfds, uin ? in : 0, uout ? out : 0, uex ? ex : 0,
                    timeout_ms, &left);
    if (r < 0) {
        return r;
    }
    if ((uin && (err = store(uin, in, bytes)) < 0) ||
        (uout && (err = store(uout, out, bytes)) < 0) ||
        (uex && (err = store(uex, ex, bytes)) < 0)) {
        return err;
    }
    if (utv) {
        /* What was not used, as Linux writes it back. */
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        err = store(utv, &tv, sizeof(tv));
        if (err < 0) {
            return err;
        }
    }
    return r;
}

/* --- sockets ---------------------------------------------------------- */

#define SK_INET 1
#define SK_UNIX 2

/* What kind of socket a descriptor is, or -EBADF / -ENOTSOCK. */
static int sock_kind(int fd, struct file **fp)
{
    struct file *f = fd_get(fd);

    if (!f) {
        return -EBADF;
    }
    *fp = f;
    if (sock_is(f)) {
        return SK_INET;
    }
    if (usock_is(f)) {
        return SK_UNIX;
    }
    return -ENOTSOCK;
}

/* An AF_INET address from the caller, `len` bytes of it. */
static int fetch_sin(struct sockaddr_in *sa, u32 uaddr, u32 len)
{
    if (!uaddr) {
        return -EFAULT;
    }
    if (len < sizeof(*sa)) {
        return -EINVAL;
    }
    return fetch(sa, uaddr, sizeof(*sa));
}

/*
 * An address back to the caller, Linux's way: *ulen says how much room
 * there is, as much as fits is copied, and *ulen is set to the address's
 * real size -- which is how a caller finds out it gave too little.
 */
static int store_addr(u32 uaddr, u32 ulen, const void *sa, u32 salen)
{
    u32 room;
    int err;

    if (!uaddr || !ulen) {
        return 0;
    }
    err = fetch(&room, ulen, sizeof(room));
    if (err < 0) {
        return err;
    }
    if ((s32)room < 0) {
        return -EINVAL;
    }
    if (room) {
        err = store(uaddr, sa, room < salen ? room : salen);
        if (err < 0) {
            return err;
        }
    }
    return store(ulen, &salen, sizeof(salen));
}

static __attribute__((noinline))
int do_socketpair(int domain, int type, int protocol, u32 usv)
{
    int sv[2], err;

    if (domain != AF_UNIX) {
        return domain == AF_INET ? -EOPNOTSUPP : -EAFNOSUPPORT;
    }
    if (protocol) {
        return -EPROTONOSUPPORT;
    }
    err = usock_pair(type, sv);
    if (err < 0) {
        return err;
    }
    err = store(usv, sv, sizeof(sv));
    if (err < 0) {
        fd_close(sv[0]);
        fd_close(sv[1]);
    }
    return err;
}

static __attribute__((noinline))
int do_bindconnect(u32 nr, int fd, u32 uaddr, u32 len)
{
    struct sockaddr_in sa;
    struct file *f;
    int k = sock_kind(fd, &f), err;

    if (k < 0) {
        return k;
    }
    if (k == SK_UNIX) {
        return nr == __NR_connect ? -EISCONN : -EINVAL;
    }
    err = fetch_sin(&sa, uaddr, len);
    if (err < 0) {
        return err;
    }
    return nr == __NR_bind ? sock_bind(f, &sa) : sock_connect(f, &sa);
}

static __attribute__((noinline))
int do_listen(int fd, int backlog)
{
    struct file *f;
    int k = sock_kind(fd, &f);

    if (k < 0) {
        return k;
    }
    return k == SK_UNIX ? -EOPNOTSUPP : sock_listen(f, backlog);
}

static __attribute__((noinline))
int do_accept4(int fd, u32 uaddr, u32 ulen, int flags)
{
    struct sockaddr_in sa;
    struct file *f;
    int k = sock_kind(fd, &f), nfd, err;

    if (k < 0) {
        return k;
    }
    if (k == SK_UNIX) {
        return -EOPNOTSUPP;
    }
    nfd = sock_accept(f, &sa, flags);
    if (nfd < 0) {
        return nfd;
    }
    err = store_addr(uaddr, ulen, &sa, sizeof(sa));
    if (err < 0) {
        fd_close(nfd);
        return err;
    }
    return nfd;
}

static __attribute__((noinline))
int do_sockname(int fd, u32 uaddr, u32 ulen, int peer)
{
    struct sockaddr_in sa;
    struct file *f;
    int k = sock_kind(fd, &f), err;

    if (k < 0) {
        return k;
    }
    if (k == SK_UNIX) {
        /* An unnamed Unix socket: the family, and nothing else. */
        struct sockaddr un;

        memset(&un, 0, sizeof(un));
        un.sa_family = AF_UNIX;
        return store_addr(uaddr, ulen, &un, sizeof(un.sa_family));
    }
    err = sock_name(f, &sa, peer);
    if (err < 0) {
        return err;
    }
    return store_addr(uaddr, ulen, &sa, sizeof(sa));
}

static __attribute__((noinline))
int do_setsockopt(int fd, int level, int name, u32 uval, u32 len)
{
    u8 val[16];
    struct file *f;
    int k = sock_kind(fd, &f), err;

    if (k < 0) {
        return k;
    }
    if (len > sizeof(val)) {
        len = sizeof(val);
    }
    err = fetch(val, uval, len);
    if (err < 0) {
        return err;
    }
    if (k == SK_UNIX) {
        return level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF)
               ? 0 : -ENOPROTOOPT;
    }
    return sock_setopt(f, level, name, val, len);
}

static __attribute__((noinline))
int do_getsockopt(int fd, int level, int name, u32 uval, u32 ulen)
{
    u8 val[16];
    u32 len;
    struct file *f;
    int k = sock_kind(fd, &f), err;

    if (k < 0) {
        return k;
    }
    err = fetch(&len, ulen, sizeof(len));
    if (err < 0) {
        return err;
    }
    if ((s32)len < 0) {
        return -EINVAL;
    }
    if (len > sizeof(val)) {
        len = sizeof(val);
    }
    if (k == SK_UNIX) {
        int v;

        if (level != SOL_SOCKET) {
            return -ENOPROTOOPT;
        }
        if (name == SO_TYPE) {
            v = SOCK_STREAM;
        } else if (name == SO_ERROR) {
            v = 0;
        } else {
            return -ENOPROTOOPT;
        }
        if (len < sizeof(int)) {
            return -EINVAL;
        }
        memcpy(val, &v, sizeof(v));
        len = sizeof(v);
    } else {
        err = sock_getopt(f, level, name, val, &len);
        if (err < 0) {
            return err;
        }
    }
    err = store(uval, val, len);
    if (err < 0) {
        return err;
    }
    return store(ulen, &len, sizeof(len));
}

/*
 * Stream data between the caller's memory and a socket, a page at a
 * time. A send carries on until it is all gone or the socket says stop;
 * a receive stops once it has data and nothing more is waiting, unless
 * MSG_WAITALL asks it to fill the buffer. A peek looks at one page.
 */
static s32 sock_xfer(struct file *f, int k, u32 ubuf, u32 len, int flags,
                     int sending)
{
    s32 total = 0;

    if (!from_program()) {
        void *p = (void *)ubuf;

        if (sending) {
            return k == SK_INET ? sock_send(f, p, len, flags, 0)
                                : usock_send(f, p, len, flags);
        }
        return k == SK_INET ? sock_recv(f, p, len, flags, 0, 0)
                            : usock_recv(f, p, len, flags);
    }
    while (len > 0) {
        u32 n = len;
        void *kp = uaccess_chunk(ubuf, &n, !sending);
        s32 got;

        if (!kp) {
            return total > 0 ? total : -EFAULT;
        }
        pmm_ref(PAGE_ALIGN_DOWN((u32)kp));      /* pinned: see rw_user */
        if (sending) {
            got = k == SK_INET ? sock_send(f, kp, n, flags, 0)
                               : usock_send(f, kp, n, flags);
        } else {
            got = k == SK_INET ? sock_recv(f, kp, n, flags, 0, 0)
                               : usock_recv(f, kp, n, flags);
        }
        pmm_free(PAGE_ALIGN_DOWN((u32)kp));
        if (got < 0) {
            return total > 0 ? total : got;
        }
        total += got;
        if ((u32)got < n) {
            break;
        }
        ubuf += n;
        len -= n;
        if (!sending && ((flags & MSG_PEEK) ||
                         (!(flags & MSG_WAITALL) &&
                          !(f->ops->poll(f) & POLLIN)))) {
            break;
        }
    }
    return total;
}

static __attribute__((noinline))
s32 do_sendto(int fd, u32 ubuf, u32 len, int flags, u32 uaddr, u32 alen)
{
    struct sockaddr_in sa;
    struct file *f;
    int k = sock_kind(fd, &f), err;

    if (k < 0) {
        return k;
    }
    if (uaddr && k == SK_INET) {
        err = fetch_sin(&sa, uaddr, alen);
        if (err < 0) {
            return err;
        }
    }
    /* A datagram goes whole: gathered here, then sent once. */
    if (k == SK_INET && sock_dgram(f)) {
        static u8 dbuf[1500];   /* one sender at a time: kernel code is
                                 * not preempted and this does not sleep
                                 * between the copy and the send */
        if (len > sizeof(dbuf)) {
            return -EMSGSIZE;
        }
        err = fetch(dbuf, ubuf, len);
        if (err < 0) {
            return err;
        }
        return sock_send(f, dbuf, len, flags, uaddr ? &sa : 0);
    }
    if (uaddr && k == SK_INET) {
        return sock_send(f, 0, 0, flags, &sa);   /* EISCONN, properly */
    }
    return sock_xfer(f, k, ubuf, len, flags, 1);
}

static __attribute__((noinline))
s32 do_recvfrom(int fd, u32 ubuf, u32 len, int flags, u32 uaddr, u32 ualen)
{
    struct sockaddr_in sa;
    struct file *f;
    int k = sock_kind(fd, &f), err, trunc;
    s32 got;

    if (k < 0) {
        return k;
    }
    if (k == SK_INET && sock_dgram(f)) {
        static u8 dbuf[1500];
        u32 n = len < sizeof(dbuf) ? len : sizeof(dbuf);

        got = sock_recv(f, dbuf, n, flags, &sa, &trunc);
        if (got < 0) {
            return got;
        }
        err = store(ubuf, dbuf, (u32)got < n ? (u32)got : n);
        if (err < 0) {
            return err;
        }
        err = store_addr(uaddr, ualen, &sa, sizeof(sa));
        return err < 0 ? err : got;
    }
    got = sock_xfer(f, k, ubuf, len, flags, 0);
    if (got >= 0 && uaddr) {
        if (k == SK_INET) {
            sock_name(f, &sa, 1);
            err = store_addr(uaddr, ualen, &sa, sizeof(sa));
        } else {
            u32 zero = 0;

            err = store(ualen, &zero, sizeof(zero));
        }
        if (err < 0) {
            return err;
        }
    }
    return got;
}

/*
 * sendmsg and recvmsg: gather and scatter over an iovec, with an
 * optional address. No ancillary data -- msg_controllen comes back 0,
 * with MSG_CTRUNC if the caller offered room for some.
 */
#define IOV_MAX_HERE    16

static __attribute__((noinline))
s32 do_msg(int fd, u32 umsg, int flags, int sending)
{
    struct msghdr m;
    struct iovec iov[IOV_MAX_HERE];
    struct file *f;
    int k = sock_kind(fd, &f), err;
    u32 i, total = 0;
    s32 r = 0;

    if (k < 0) {
        return k;
    }
    err = fetch(&m, umsg, sizeof(m));
    if (err < 0) {
        return err;
    }
    if (m.msg_iovlen > IOV_MAX_HERE) {
        return -EMSGSIZE;
    }
    err = fetch(iov, (u32)m.msg_iov, m.msg_iovlen * sizeof(struct iovec));
    if (err < 0) {
        return err;
    }

    if (k == SK_INET && sock_dgram(f)) {
        static u8 dbuf[1500];
        struct sockaddr_in sa;
        int trunc = 0;
        u32 at = 0;

        if (sending) {
            for (i = 0; i < m.msg_iovlen; i++) {
                if (at + iov[i].iov_len > sizeof(dbuf)) {
                    return -EMSGSIZE;
                }
                err = fetch(dbuf + at, (u32)iov[i].iov_base, iov[i].iov_len);
                if (err < 0) {
                    return err;
                }
                at += iov[i].iov_len;
            }
            if (m.msg_name) {
                err = fetch_sin(&sa, (u32)m.msg_name, m.msg_namelen);
                if (err < 0) {
                    return err;
                }
            }
            return sock_send(f, dbuf, at, flags, m.msg_name ? &sa : 0);
        }
        r = sock_recv(f, dbuf, sizeof(dbuf), flags & ~MSG_TRUNC, &sa, &trunc);
        if (r < 0) {
            return r;
        }
        for (i = 0; i < m.msg_iovlen && at < (u32)r; i++) {
            u32 n = (u32)r - at < iov[i].iov_len ? (u32)r - at
                                                 : iov[i].iov_len;

            err = store((u32)iov[i].iov_base, dbuf + at, n);
            if (err < 0) {
                return err;
            }
            at += n;
        }
        m.msg_flags = (at < (u32)r || trunc) ? MSG_TRUNC : 0;
        if (m.msg_name) {
            u32 room = m.msg_namelen < sizeof(sa) ? m.msg_namelen : sizeof(sa);

            err = store((u32)m.msg_name, &sa, room);
            if (err < 0) {
                return err;
            }
            m.msg_namelen = sizeof(sa);
        }
        r = (flags & MSG_TRUNC) ? r : (s32)at;
    } else {
        for (i = 0; i < m.msg_iovlen; i++) {
            s32 n = sock_xfer(f, k, (u32)iov[i].iov_base, iov[i].iov_len,
                              flags, sending);

            if (n < 0) {
                if (total == 0) {
                    return n;
                }
                break;
            }
            total += (u32)n;
            if ((u32)n < iov[i].iov_len) {
                break;
            }
        }
        r = (s32)total;
        m.msg_flags = 0;
        m.msg_namelen = 0;
    }
    if (!sending) {
        if (m.msg_controllen) {
            m.msg_flags |= MSG_CTRUNC;
        }
        m.msg_controllen = 0;
        err = store(umsg, &m, sizeof(m));
        if (err < 0) {
            return err;
        }
    }
    return r;
}

static __attribute__((noinline))
int do_shutdown(int fd, int how)
{
    struct file *f;
    int k = sock_kind(fd, &f);

    if (k < 0) {
        return k;
    }
    return k == SK_UNIX ? usock_shutdown(f, how) : sock_shutdown(f, how);
}

/* --- the network --------------------------------------------------- */

static int do_netctl(int cmd, u32 arg, u32 p)
{
    struct netif *n = net_if();

    switch (cmd) {
    case NETCTL_INFO: {
        struct netinfo out;

        memset(&out, 0, sizeof(out));
        if (arg == 1) {
            n = net_lo();
            strncpy(out.name, "lo", sizeof(out.name) - 1);
            out.loopback = 1;
        } else if (arg != 0 || !n->dev) {
            return -ENODEV;
        } else {
            strncpy(out.name, n->dev->name, sizeof(out.name) - 1);
            out.dns = dhcp_dns();
        }
        memcpy(out.mac, n->mac, 6);
        out.ip = n->ip;
        out.netmask = n->netmask;
        out.gateway = n->gateway;
        out.up = (u32)n->up;
        out.rx_packets = n->rx_packets;
        out.tx_packets = n->tx_packets;
        out.rx_dropped = n->rx_dropped;
        out.tx_errors = n->tx_errors;
        return store(p, &out, sizeof(out));
    }

    case NETCTL_UP:
    case NETCTL_DOWN:
        if (arg == 1) {
            n = net_lo();
        } else if (arg != 0 || !n->dev) {
            return -ENODEV;
        }
        /*
         * THE DRIVER IS TOLD, not just the flag set.
         *
         * This used to set n->up and nothing else, so `ifconfig eth0
         * down` followed by `ifconfig eth0 up` changed a boolean and
         * never touched the hardware -- which means it could not
         * recover a card that had got itself stuck, and that is the
         * one thing somebody types it for. smc_up() resets the packet
         * MMU, which is what hands back every page the chip is
         * holding.
         *
         * Masked, because the driver owns the chip's bank and pointer
         * registers and net_drain() reaches for the same ones from the
         * timer interrupt. net_tx() masks for exactly this reason.
         */
        if (n->dev) {
            u16 sr = irq_save();
            int err = 0;

            if (cmd == NETCTL_UP) {
                if (n->dev->up) {
                    err = n->dev->up(n->dev);
                }
            } else {
                if (n->dev->down) {
                    err = n->dev->down(n->dev);
                }
            }
            irq_restore(sr);
            if (err < 0) {
                return err;
            }
        }
        /* Down, a card's frames are left on it and its sends refused
         * (net_drain, net_tx); lo's sends are refused (net_loopback). */
        n->up = cmd == NETCTL_UP;
        return 0;

    case NETCTL_SETADDR: {
        struct netaddr a;
        int err = fetch(&a, p, sizeof(a));

        if (err < 0) {
            return err;
        }
        net_set_addr(a.ip, a.netmask, a.gateway);
        return 0;
    }

    case NETCTL_ARPING: {
        u8 mac[6];

        if (!n->dev) {
            return -ENODEV;
        }
        return arp_resolve(arg, mac);
    }

    case NETCTL_PING: {
        u32 rtt = 0;
        int err;

        /* No card is no reason to refuse: 127.0.0.1 needs none. */
        err = icmp_ping(arg, 2000, &rtt);
        if (err < 0) {
            return err;
        }
        return store(p, &rtt, sizeof(rtt));
    }

    case NETCTL_CONN: {
        struct conninfo out;
        struct tcpcb *c = tcp_nth((int)arg);

        if (!c) {
            return -ENOENT;
        }
        memset(&out, 0, sizeof(out));
        out.local_ip = c->local_ip;
        out.remote_ip = c->remote_ip;
        out.local_port = c->local_port;
        out.remote_port = c->remote_port;
        out.state = (u32)c->state;
        out.txq = c->sndlen;
        out.rxq = tcp_available(c);
        out.cwnd = c->cwnd;
        out.rtt_ms = c->rtt_valid ? c->srtt_ms : 0;
        strncpy(out.state_name, tcp_state_name(c->state),
                sizeof(out.state_name) - 1);
        out.flags = (c->ws_ok ? CONN_WS : 0) | (c->ts_ok ? CONN_TS : 0) |
                    (c->sack_ok ? CONN_SACK : 0) |
                    (c->keepalive ? CONN_KEEP : 0);
        out.snd_wscale = c->snd_wscale;
        out.rcv_wscale = c->rcv_wscale;
        out.snd_wnd = c->snd_wnd;
        out.max_snd_wnd = c->max_snd_wnd;
        out.rexmit_segs = c->rexmit_segs;
        out.rexmit_bytes = c->rexmit_bytes;
        out.keep_sent = c->keep_sent;
        return store(p, &out, sizeof(out));
    }

    case NETCTL_TCPLOSS:
        tcp_set_loss(arg);
        return 0;

    case NETCTL_TCPOPTS:
        tcp_set_disabled(arg);
        return 0;

    case NETCTL_DHCP: {
        struct netaddr out;
        int err;

        if (!n->dev) {
            return -ENODEV;
        }
        err = dhcp_configure();
        if (err < 0) {
            return err;
        }
        out.ip = n->ip;
        out.netmask = n->netmask;
        out.gateway = n->gateway;
        return store(p, &out, sizeof(out));
    }

    case NETCTL_ARP: {
        struct arpinfo out;
        ip4_t ip;
        u8 mac[6];
        u32 age;

        memset(&out, 0, sizeof(out));
        if (!arp_entry((int)arg, &ip, mac, &age)) {
            return -ENOENT;
        }
        out.ip = ip;
        memcpy(out.mac, mac, 6);
        out.age_ms = age;
        return store(p, &out, sizeof(out));
    }

    default:
        return -EINVAL;
    }
}

/* --- jobs, which are now just this task's children -------------------- */

static int info_state(int state)
{
    switch (state) {
    case TASK_READY:
    case TASK_RUNNING: return JOB_S_RUNNING;
    case TASK_BLOCKED: return JOB_S_BLOCKED;
    case TASK_STOPPED: return JOB_S_STOPPED;
    case TASK_ZOMBIE:  return JOB_S_DONE;
    default:           return JOB_S_NEW;
    }
}

/* The Nth child of the calling task. */
static struct task *nth_child(int n)
{
    struct task *t;
    int i;

    for (i = 0; (t = task_nth(i)) != 0; i++) {
        if (t->parent != current) {
            continue;
        }
        if (n-- == 0) {
            return t;
        }
    }
    return 0;
}

/*
 * setpgid(), with POSIX's rules as far as this system has the things
 * they talk about (there are no sessions): a task may move itself or a
 * child of its own, into a group of its own number or into a group that
 * already exists.
 */
static int do_setpgid(int pid, int pgid)
{
    struct task *t = pid ? task_find(pid) : current;
    struct task *m;
    int i;

    if (!t || (t != current && t->parent != current)) {
        return -ESRCH;
    }
    if (pgid < 0) {
        return -EINVAL;
    }
    /* POSIX: within one session only, and never a session's leader. */
    if (t->sid != current->sid || t->sid == t->pid) {
        return -EPERM;
    }
    if (pgid == 0) {
        pgid = t->pid;
    }
    if (pgid != t->pid) {
        for (i = 0; (m = task_nth(i)) != 0; i++) {
            if (m->pgid == pgid && m->sid == t->sid && m->state != TASK_ZOMBIE) {
                break;
            }
        }
        if (!m) {
            return -EPERM;
        }
    }
    t->pgid = pgid;
    return 0;
}

/*
 * SIGCONT to the members of a group that are actually STOPPED, and to
 * nobody else. Sending it to a task that is merely being brought to the
 * foreground leaves a signal pending on it -- and anything that checks
 * for one before blocking, which the whole network stack does, then
 * returns immediately. Every program's first packet was lost that way.
 */
static void continue_group(int pgid)
{
    struct task *t;
    int i;

    for (i = 0; (t = task_nth(i)) != 0; i++) {
        if (t->pgid == pgid && t->state == TASK_STOPPED) {
            signal_send(t, SIGCONT);
        }
    }
}

static int do_jobctl(int cmd, int arg, u32 p)
{
    struct task *t;

    switch (cmd) {
    case JOBCTL_ALL:
    case JOBCTL_INFO: {
        struct job_info out;

        /* INFO walks this task's children, which is what `jobs` wants;
         * ALL walks everything, which is what `ps` wants. */
        t = (cmd == JOBCTL_ALL) ? task_nth(arg) : nth_child(arg);
        if (!t) {
            return -ENOENT;
        }
        memset(&out, 0, sizeof(out));
        out.id = t->pid;
        out.state = info_state(t->state);
        out.background = t->background;
        out.status = t->exit_status;
        out.signalled = t->signalled;
        out.ppid = t->parent ? t->parent->pid : 0;
        strncpy(out.cmd, t->cmd, sizeof(out.cmd) - 1);
        out.cmd[sizeof(out.cmd) - 1] = '\0';
        return store(p, &out, sizeof(out));
    }

    case JOBCTL_WHO: {
        struct who_info out;
        const char *name;

        t = task_nth(arg);
        if (!t) {
            return -ENOENT;
        }
        memset(&out, 0, sizeof(out));
        out.pid = t->pid;
        out.ppid = t->parent ? t->parent->pid : 0;
        out.sid = t->sid;
        out.uid = t->uid;       /* the REAL uid: who, not what it may do */
        out.start = t->start;
        /* The terminal is whatever descriptor 0 is open on, which is
         * what a session's input actually comes from. A task with no
         * descriptor 0, or one that is not a device, leaves tty empty
         * rather than claiming a terminal it has not got. */
        if (t->files && t->files->fd[0]) {
            name = dev_char_name(t->files->fd[0]);
            if (name) {
                strncpy(out.tty, name, sizeof(out.tty) - 1);
            }
        }
        strncpy(out.name, t->name, sizeof(out.name) - 1);
        strncpy(out.cmd, t->cmd, sizeof(out.cmd) - 1);
        return store(p, &out, sizeof(out));
    }

    case JOBCTL_FG:
        /*
         * Bring it to the foreground: let it run again if it was
         * stopped, take the terminal, and wait. The waiting is what
         * makes it foreground -- everything else about the task is the
         * same either way.
         *
         * Job 0 means the caller is taking the terminal back, which is
         * what a shell does when whatever it was waiting for has
         * finished.
         */
        if (arg == 0) {
            tty_set_foreground(current->pgid);
            return 0;
        }
        t = task_find(arg);
        if (!t || t->parent != current) {
            return -ECHILD;
        }
        t->background = 0;
        /* The job's whole GROUP gets the terminal: every command of a
         * pipeline, and anything those started. */
        tty_set_foreground(t->pgid);
        continue_group(t->pgid);
        return 0;

    case JOBCTL_BG:
        t = task_find(arg);
        if (!t || t->parent != current) {
            return -ECHILD;
        }
        t->background = 1;
        /* The terminal goes back to whoever asked, because a background
         * job is precisely one that does not have it. */
        tty_set_foreground(current->pgid);
        continue_group(t->pgid);
        return 0;

    case JOBCTL_REAP: {
        int i;

        for (i = 0; (t = task_nth(i)) != 0; ) {
            if (t->parent == current && t->state == TASK_ZOMBIE) {
                task_reap(t);
                i = 0;          /* the table shifted under us */
            } else {
                i++;
            }
        }
        return 0;
    }

    case JOBCTL_DROP:
        t = task_find(arg);
        if (!t || t->parent != current) {
            return -ECHILD;
        }
        if (t->state != TASK_ZOMBIE) {
            return -EBUSY;
        }
        task_reap(t);
        return 0;

    default:
        return -EINVAL;
    }
}

static s32 do_syscall(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5,
                      u32 a6, struct pt_regs *regs)
{

    switch (nr) {
    case __NR_open: {
        char path[PATH_MAX];
        int err = fetch_str(path, a1, sizeof(path));

        if (err < 0) {
            return err;
        }
        return fd_open(path, (int)a2);
    }

    case __NR_close:
        return fd_close((int)a1);

    case __NR_read:
        return rw_user((int)a1, a2, a3, 0);

    case __NR_write:
        return rw_user((int)a1, a2, a3, 1);

    case __NR_lseek:
        return fd_lseek((int)a1, (s32)a2, (int)a3);

    case __NR_ioctl:
        return do_ioctl((int)a1, a2, a3);

    case __NR_unlink: {
        char path[PATH_MAX];
        int err = fetch_str(path, a1, sizeof(path));

        if (err < 0) {
            return err;
        }
        return vfs_unlink(path);
    }

    case __NR_mkdir:
    case __NR_rmdir:
    case __NR_chdir: {
        char path[PATH_MAX];
        int err = fetch_str(path, a1, sizeof(path));

        if (err < 0) {
            return err;
        }
        if (nr == __NR_mkdir) {
            return vfs_mkdir(path);
        }
        if (nr == __NR_rmdir) {
            return vfs_rmdir(path);
        }
        return vfs_chdir(path);
    }

    case __NR_getcwd: {
        const char *cwd = vfs_getcwd();
        u32 n = (u32)strlen(cwd) + 1;

        if (a2 < n) {
            return -ERANGE;
        }
        {
            int err = store(a1, cwd, n);

            return err < 0 ? err : (s32)n;
        }
    }

    case __NR_rename: {
        char from[PATH_MAX], to[PATH_MAX];
        int err = fetch_str(from, a1, sizeof(from));

        if (err < 0) {
            return err;
        }
        err = fetch_str(to, a2, sizeof(to));
        if (err < 0) {
            return err;
        }
        return vfs_rename(from, to);
    }

    case __NR_stat: {
        char path[PATH_MAX];
        struct stat st;
        int err = fetch_str(path, a1, sizeof(path));

        if (err < 0) {
            return err;
        }
        err = vfs_stat(path, &st);
        if (err < 0) {
            return err;
        }
        return store(a2, &st, sizeof(st));
    }

    case __NR_fstat: {
        struct stat st;
        int err = vfs_fstat((int)a1, &st);

        if (err < 0) {
            return err;
        }
        return store(a2, &st, sizeof(st));
    }

    case __NR_access: {
        char path[PATH_MAX];
        int err = fetch_str(path, a1, sizeof(path));

        if (err < 0) {
            return err;
        }
        return vfs_access(path, (int)a2);
    }

    case __NR_pipe: {
        int fds[2];
        int err = pipe_create(fds, 0);

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

    case __NR_fcntl:
        return fd_fcntl((int)a1, (int)a2, a3);

    case __NR_dup:
        return fd_dup((int)a1);

    case __NR_dup2:
        return fd_dup2((int)a1, (int)a2);

    case __NR_getdents: {
        struct dirent d;
        int err = vfs_readdir((int)a1, &d);

        if (err < 0) {
            return err;
        }
        err = store(a2, &d, sizeof(d));
        return err < 0 ? err : 0;
    }

    case __NR_statfs: {
        struct statfs sf;
        int err = vfs_statfs(&sf);

        if (err < 0) {
            return err;
        }
        return store(a1, &sf, sizeof(sf));
    }

    /*
     * fdatasync is fsync here: there is no separate metadata journal
     * for it to skip. See uapi.h -- it exists because SQLite prefers
     * it, and an ENOSYS from it reads as "disk I/O error".
     */
    case __NR_fsync:
    case __NR_fdatasync:
    case __NR_sync:
        return vfs_sync();

    case __NR_time: {
        time_t now = clock_now();

        if (a1) {
            int err = store(a1, &now, sizeof(now));

            if (err < 0) {
                return err;
            }
        }
        return (s32)now;
    }

    case __NR_stime: {
        time_t t;
        int err = fetch(&t, a1, sizeof(t));

        if (err < 0) {
            return err;
        }
        return do_stime(&t);
    }

    case __NR_sysinfo: {
        struct sysinfo si;
        memset(&si, 0, sizeof(si));
        si.uptime = timer_jiffies() / HZ;
        si.totalram = pmm_total();
        si.freeram = pmm_available();
        si.bufferram = textcache_idle();
        {
            struct swapstats s;

            swap_stats(&s);
            si.totalswap = s.slots;
            si.freeswap = s.slots - s.used;
        }
        si.mem_unit = (u32)PAGE_SIZE;
        si.procs = (u16)task_count();
        return store(a1, &si, sizeof(si));
    }

    case __NR_uname: {
        struct utsname u;
        int err = do_uname(&u);

        if (err < 0) {
            return err;
        }
        return store(a1, &u, sizeof(u));
    }

    case __NR_reboot:
        do_reboot((int)a1);
        return 0;               /* not reached */

    case __NR_fork: {
        struct task *t = task_fork(regs);

        /* Linux's two answers: EAGAIN when the task table is full,
         * ENOMEM when memory is. */
        return t ? t->pid : (task_count() >= TASK_MAX ? -EAGAIN : -ENOMEM);
    }

    case __NR_execve:
        return do_execve(a1, a2, a3, regs);

    case __NR_spawn:
        return do_spawn(a1, (int)a2, a3, a4);

    case __NR_jobctl:
        return do_jobctl((int)a1, (int)a2, a3);

    case __NR_netctl:
        return do_netctl((int)a1, a2, a3);

    case __NR_fsctl:
        switch (a1) {
        case FSCTL_CHECK: {
            struct fsck_report r;
            int err = vfs_check((int)a2, &r);

            if (err < 0) {
                return err;
            }
            return a3 ? store(a3, &r, sizeof(r)) : 0;
        }
        case FSCTL_LABEL: {
            struct fslabel l;
            int err = vfs_label(&l);

            if (err < 0) {
                return err;
            }
            return a3 ? store(a3, &l, sizeof(l)) : 0;
        }
        default:
            return -EINVAL;
        }

    case __NR_memctl:
        if (a1 == MEMCTL_STATS) {
            struct memstats m;
            struct tc_stats t;

            textcache_stats(&t);
            m.pages_total = pmm_total();
            m.pages_free = pmm_available();
            m.tc_cached = t.cached;
            m.tc_hits = t.hits;
            m.tc_misses = t.misses;
            m.tc_evicted = t.evicted;
            m.tc_forgotten = t.forgotten;
            {
                struct vm_stats v;
                struct swapstats s;

                vm_stats(&v);
                swap_stats(&s);
                m.faults_zero = v.faults_zero;
                m.faults_cow = v.faults_cow;
                m.faults_swapin = v.faults_swapin;
                m.evicted = v.evicted;
                m.swap_slots = s.slots;
                m.swap_used = s.used;
                m.pageouts = s.pageouts;
                m.pageins = s.pageins;
            }
            return store(a3, &m, a2 < sizeof(m) ? a2 : sizeof(m));
        }
        if (a1 == MEMCTL_PAGE) {
            struct pageinfo pi;

            if (!current->as) {
                return -ENODEV;
            }
            pi.pa = PAGE_ALIGN_DOWN(vm_translate(current->as, a2, 0));
            pi.refs = pi.pa ? pmm_refcount(pi.pa) : 0;
            pi.writable = vm_may_write(current->as, a2);
            return store(a3, &pi, sizeof(pi));
        }
        return -EINVAL;

    case __NR_kstat:
        if (a1 == KSTAT_IRQ) {
            struct irqstats is;

            mfp_counts(is.count);
            is.spurious = mfp_spurious();
            is.tty_overruns = tty_overruns();
            ata_counts(&is.disk_slept, &is.disk_polled);
            return store(a3, &is, a2 < sizeof(is) ? a2 : sizeof(is));
        }
        if (a1 == KSTAT_STACK) {
            struct kstackstats ks;

            memset(&ks, 0, sizeof(ks));
            task_kstack_stats(&ks.size, &ks.max_used, ks.max_task,
                              sizeof(ks.max_task));
            return store(a3, &ks, a2 < sizeof(ks) ? a2 : sizeof(ks));
        }
        if (a1 == KSTAT_DISK_DELAY) {
            ata_set_delay(a2 > 1000 ? 1000 : a2);
            return 0;
        }
        return -EINVAL;

    case __NR_socket:
        return sock_create((int)a1, (int)a2, (int)a3);
    case __NR_socketpair:
        return do_socketpair((int)a1, (int)a2, (int)a3, a4);
    case __NR_bind:
    case __NR_connect:
        return do_bindconnect(nr, (int)a1, a2, a3);
    case __NR_listen:
        return do_listen((int)a1, (int)a2);
    case __NR_accept4:
        return do_accept4((int)a1, a2, a3, (int)a4);
    case __NR_getsockname:
    case __NR_getpeername:
        return do_sockname((int)a1, a2, a3, nr == __NR_getpeername);
    case __NR_setsockopt:
        return do_setsockopt((int)a1, (int)a2, (int)a3, a4, a5);
    case __NR_getsockopt:
        return do_getsockopt((int)a1, (int)a2, (int)a3, a4, a5);
    case __NR_sendto:
        return do_sendto((int)a1, a2, a3, (int)a4, a5, a6);
    case __NR_recvfrom:
        return do_recvfrom((int)a1, a2, a3, (int)a4, a5, a6);
    case __NR_sendmsg:
        return do_msg((int)a1, a2, (int)a3, 1);
    case __NR_recvmsg:
        return do_msg((int)a1, a2, (int)a3, 0);
    case __NR_shutdown:
        return do_shutdown((int)a1, (int)a2);

    case __NR_times: {
        struct tms tms;
        int err;

        if (a1) {
            tms.tms_utime = current->utime;
            tms.tms_stime = current->stime;
            tms.tms_cutime = current->cutime;
            tms.tms_cstime = current->cstime;
            err = store(a1, &tms, sizeof(tms));
            if (err < 0) {
                return err;
            }
        }
        return (s32)timer_jiffies();
    }

    case __NR_gettimeofday: {
        struct timeval tv;

        clock_get(&tv);
        /* The timezone is accepted and ignored, as it is everywhere. */
        return a1 ? store(a1, &tv, sizeof(tv)) : 0;
    }

    case __NR_settimeofday: {
        struct timeval tv;
        int err;

        if (!a1) {
            return 0;               /* a timezone alone: nothing to do */
        }
        err = fetch(&tv, a1, sizeof(tv));
        if (err < 0) {
            return err;
        }
        return clock_set(&tv);
    }

    case __NR_alarm: {
        struct itimerval old, set;
        u32 left;

        itimer_get(ITIMER_REAL, &old);
        memset(&set, 0, sizeof(set));
        set.it_value.tv_sec = (s32)a1;
        if (itimer_set(ITIMER_REAL, &set) < 0) {
            return -EINVAL;
        }
        /* Whole seconds left of the old one, rounded up, as Linux: an
         * alarm that has not fired never reports 0. */
        left = (u32)old.it_value.tv_sec;
        if (old.it_value.tv_usec) {
            left++;
        }
        return (s32)left;
    }

    case __NR_setitimer: {
        struct itimerval in, old;
        int err;

        err = itimer_get((int)a1, &old);
        if (err < 0) {
            return err;
        }
        if (a2) {
            err = fetch(&in, a2, sizeof(in));
            if (err < 0) {
                return err;
            }
            err = itimer_set((int)a1, &in);
            if (err < 0) {
                return err;
            }
        }
        return a3 ? store(a3, &old, sizeof(old)) : 0;
    }

    case __NR_getitimer: {
        struct itimerval cur;
        int err = itimer_get((int)a1, &cur);

        if (err < 0) {
            return err;
        }
        return store(a2, &cur, sizeof(cur));
    }

    case __NR_nanosleep: {
        struct timespec ts;
        const struct timespec *req = &ts;
        u32 ms, started, elapsed;
        int woken;
        int err = fetch(&ts, a1, sizeof(ts));

        if (err < 0) {
            return err;
        }
        if (req->tv_nsec >= 1000000000UL) {
            return -EINVAL;
        }
        /* Milliseconds is as fine as a 10 ms tick can express, and
         * rounding to it before the tick calculation keeps the
         * arithmetic clear of overflow. */
        ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;

        /*
         * Sleep on a wait queue that nothing ever wakes, so the only
         * two ways out are the timeout and a signal.
         *
         * This used to call timer_sleep_ms(), which loops on STOP in
         * supervisor mode. That was right when there was one program:
         * halting the processor beats spinning, and the tick wakes it.
         * It became badly wrong the moment there was a scheduler,
         * because preemption happens only on the way back to USER mode
         * -- so a task inside this system call was never preempted, and
         * `sleep 5` stopped the whole machine for five seconds while
         * every other task sat ready. Nothing caught it because nothing
         * in the tree slept and did anything else at the same time.
         */
        started = timer_jiffies();
        woken = sleep_on_timeout(&sleep_waitq, ms);

        if (a2) {
            struct timespec rem;
            u32 left_ms = 0;

            /*
             * What is left, which is only ever non-zero when a signal
             * cut the sleep short. The old code stored zeroes and said
             * nothing could interrupt a sleep; something can now.
             */
            elapsed = (timer_jiffies() - started) * (1000 / HZ);
            if (woken && elapsed < ms) {
                left_ms = ms - elapsed;
            }
            rem.tv_sec = left_ms / 1000;
            rem.tv_nsec = (left_ms % 1000) * 1000000UL;
            err = store(a2, &rem, sizeof(rem));
            if (err < 0) {
                return err;
            }
        }

        if (woken && signal_pending(current)) {
            return -EINTR;
        }
        return 0;
    }

    case __NR_exit:
        task_exit((int)a1);     /* does not return */
        return 0;

    case __NR_getpid:
        /* The PROCESS, not the thread: every thread of one program
         * agrees about what getpid() says, and gettid() is what tells
         * them apart. For a process of one they are the same number. */
        return current->tgid;

    case __NR_getppid:
        return current->parent ? current->parent->pid : 0;

    case __NR_getpgrp:
        return current->pgid;

    case __NR_getpgid: {
        struct task *t = a1 ? task_find((int)a1) : current;

        return t ? t->pgid : -ESRCH;
    }

    case __NR_setpgid:
        return do_setpgid((int)a1, (int)a2);

    case __NR_brk:
        /* Never negative: every user address is below 0x80000000, so a
         * break can not be mistaken for an errno on the way back. */
        return (s32)vm_brk(current->as, a1);

    case __NR_mmap2:
        /* The offset is in pages, which is the point of mmap2: it
         * reaches past 4 GB of file on a 32-bit machine. Not a concern
         * here, but the conversion must not overflow either. */
        if (a6 > 0xffffffffUL / PAGE_SIZE) {
            return -EINVAL;
        }
        return do_mmap(a1, a2, a3, a4, (int)a5, a6 * PAGE_SIZE);

    case __NR_mmap: {
        struct mmap_arg_struct m;
        int err = fetch(&m, a1, sizeof(m));

        if (err < 0) {
            return err;
        }
        return do_mmap(m.addr, m.len, m.prot, m.flags, (int)m.fd, m.offset);
    }

    case __NR_munmap:
        return do_munmap(a1, a2);

    case __NR_poll:
        return do_poll(a1, a2, (s32)a3);

    case __NR__newselect:
        return do_select(a1, a2, a3, a4, a5);

    case __NR_select: {
        struct sel_arg_struct sa;
        int err = fetch(&sa, a1, sizeof(sa));

        if (err < 0) {
            return err;
        }
        return do_select(sa.n, (u32)sa.inp, (u32)sa.outp, (u32)sa.exp,
                         (u32)sa.tvp);
    }

    case __NR_mprotect:
        return do_mprotect(a1, a2, a3);

    /*
     * m68k's own call, for a program that wrote code and now wants to
     * run it. See kernel/cache.c for why it exists and why it is
     * currently a correct no-op.
     */
    case __NR_cacheflush:
        return do_cacheflush(a1, (int)a2, (int)a3, a4);

    case __NR_kill:
        return signal_kill((int)a1, (int)a2);

    case __NR_sigaction: {
        struct sigaction act, old;
        int err;

        if (a2) {
            err = fetch(&act, a2, sizeof(act));
            if (err < 0) {
                return err;
            }
        }
        err = signal_set_action((int)a1, a2 ? &act : 0, &old);
        if (err < 0) {
            return err;
        }
        return a3 ? store(a3, &old, sizeof(old)) : 0;
    }

    case __NR_sigprocmask: {
        u32 set, old;
        int err;

        if (a2) {
            err = fetch(&set, a2, sizeof(set));
            if (err < 0) {
                return err;
            }
        }
        err = signal_procmask((int)a1, a2 ? &set : 0, &old);
        if (err < 0) {
            return err;
        }
        return a3 ? store(a3, &old, sizeof(old)) : 0;
    }

    case __NR_sigpending: {
        u32 set = signal_pending_set();

        return store(a1, &set, sizeof(set));
    }

    case __NR_sigsuspend:
        return signal_suspend(a1);

    case __NR_pause:
        return signal_pause();

    case __NR_sigreturn:
        return signal_return(regs);

    case __NR_sched_yield:
        schedule();
        return 0;

    case __NR_waitpid: {
        int status = 0;
        int got = task_wait((int)a1, &status, (int)a3);

        if (got < 0) {
            return got;
        }
        if (a2) {
            int err = store(a2, &status, sizeof(status));

            if (err < 0) {
                return err;
            }
        }
        return got;
    }

    default:
        return syscall_linux(nr, a1, a2, a3, a4, a5, a6, regs);
    }
}

/* ---------------------------------------------------------------- */
/* Leaving the kernel                                                */
/*                                                                    */
/* The one place a signal is acted on and the one place a task can    */
/* be preempted, and both for the same reason: the kernel has         */
/* finished whatever it was doing, so its own state is consistent and */
/* the task's stack is a place that can be left.                      */
/*                                                                    */
/* This used to count how deep in the kernel it was, because a single */
/* program was entered from inside the shell's own spawn and its      */
/* boundaries were therefore invisible. Tasks removed the problem     */
/* rather than solving it: every task has a kernel stack of its own,  */
/* so a system call it makes is at depth one by definition.           */
/* ---------------------------------------------------------------- */

void syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5,
                      u32 a6, struct pt_regs *regs)
{
    /*
     * The result goes where the gate's final movem will find it. It is
     * stored before signals are looked at, because delivering one saves
     * these registers for sigreturn to restore -- and what it restores
     * has to include what this call returned.
     */
    current->syscall_nr = (int)nr;
    regs->d[0] = (u32)do_syscall(nr, a1, a2, a3, a4, a5, a6, regs);

    /*
     * Signals, then a possible switch -- and neither if this call came
     * from the kernel itself, which the saved SR is what says. A signal
     * that interrupted the call may restart it; see signal.c.
     */
    task_ret_to_user(regs);
    current->syscall_nr = -1;
}
