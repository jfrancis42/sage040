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
#include "mmap.h"
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
/* The trap itself                                                   */
/* ---------------------------------------------------------------- */

s32 syscall0(u32 nr)
{
    register u32 d0 __asm__("d0") = nr;

    __asm__ volatile ("trap #0" : "+d"(d0) : : "memory", "cc");
    return (s32)d0;
}

s32 syscall1(u32 nr, u32 a1)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;

    __asm__ volatile ("trap #0" : "+d"(d0) : "d"(d1) : "memory", "cc");
    return (s32)d0;
}

s32 syscall2(u32 nr, u32 a1, u32 a2)
{
    register u32 d0 __asm__("d0") = nr;
    register u32 d1 __asm__("d1") = a1;
    register u32 d2 __asm__("d2") = a2;

    __asm__ volatile ("trap #0"
                      : "+d"(d0) : "d"(d1), "d"(d2) : "memory", "cc");
    return (s32)d0;
}

s32 syscall4(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4)
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

s32 syscall3(u32 nr, u32 a1, u32 a2, u32 a3)
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

static int from_program(void)
{
    return uaccess_current() != 0;
}

static int fetch(void *dst, u32 p, u32 len)
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

static int store(u32 p, const void *src, u32 len)
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
static int fetch_str(char *dst, u32 p, u32 max)
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
        got = writing ? fd_write(fd, k, n) : fd_read(fd, k, n);
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

    /*
     * RB_HALT_SYSTEM stops the CPU and leaves the machine sitting there,
     * which is what `halt` has always done here: the emulator keeps
     * running and a harness watching the serial output is what notices.
     *
     * The other two ask for the machine to actually go away, and a
     * driver may know how to make it. On this board one does -- the
     * keyboard controller has a reset line, which is how every PC since
     * 1984 has rebooted itself. If none does, saying so and halting is
     * better than pretending, because "shutdown" that silently did
     * nothing would be worse than one that says it cannot.
     */
    if ((u32)cmd != RB_HALT_SYSTEM) {
        if (dev_poweroff() == 0) {
            /* It worked; the machine is already going. */
            for (;;) {
                halt();
            }
        }
        kputln("reboot: nothing on this board can cut the power; halting");
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
    { TCGETS,       sizeof(struct termios),       IO_OUT },
    { TCSETS,       sizeof(struct termios),       IO_IN  },
    { TCSETSW,      sizeof(struct termios),       IO_IN  },
    { TCSETSF,      sizeof(struct termios),       IO_IN  },
    { TIOCGCONS,    sizeof(struct console_info),  IO_IN | IO_OUT },
    { TIOCSCONS,    sizeof(struct console_set),   IO_IN  },
    { TIOCGPGRP,    sizeof(int),                  IO_OUT },
    { TIOCSPGRP,    sizeof(int),                  IO_IN  },
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
#define SPAWN_ARG_MAX  64

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
    char argstore[EXEC_MAX_ARGS][SPAWN_ARG_MAX];
    char *argv[EXEC_MAX_ARGS];
    char envstore[EXEC_MAX_ENV][SPAWN_ARG_MAX];
    char *envp[EXEC_MAX_ENV + 1];
    u32 uptr[EXEC_MAX_ARGS];
    int i, err, envc = 0;

    if (argc < 0 || argc > EXEC_MAX_ARGS) {
        return -E2BIG;
    }
    err = fetch_str(path, upath, sizeof(path));
    if (err < 0) {
        return err;
    }

    if (!from_program()) {
        return exec_spawn(path, argc, (char **)uargv, (char **)uenvp);
    }

    if (argc > 0) {
        err = fetch(uptr, uargv, (u32)argc * 4);
        if (err < 0) {
            return err;
        }
    }
    for (i = 0; i < argc; i++) {
        err = fetch_str(argstore[i], uptr[i], SPAWN_ARG_MAX);
        if (err < 0) {
            return err;
        }
        argv[i] = argstore[i];
    }

    /*
     * The environment the same way, one level of user pointer deeper.
     * It has to end up in kernel memory because exec_spawn copies it
     * into a DIFFERENT address space, by which time this one may not
     * even be mapped.
     */
    if (uenvp) {
        u32 eptr[EXEC_MAX_ENV + 1];

        err = fetch(eptr, uenvp, sizeof(eptr));
        if (err < 0) {
            return err;
        }
        while (envc < EXEC_MAX_ENV && eptr[envc]) {
            err = fetch_str(envstore[envc], eptr[envc], SPAWN_ARG_MAX);
            if (err < 0) {
                return err;
            }
            envp[envc] = envstore[envc];
            envc++;
        }
    }
    envp[envc] = 0;

    return exec_spawn(path, argc, argv, envp);
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

/* --- the network --------------------------------------------------- */

static int do_netctl(int cmd, u32 arg, u32 p)
{
    struct netif *n = net_if();

    switch (cmd) {
    case NETCTL_INFO: {
        struct netinfo out;

        memset(&out, 0, sizeof(out));
        if (!n->dev) {
            return -ENODEV;
        }
        strncpy(out.name, n->dev->name, sizeof(out.name) - 1);
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

        if (!n->dev) {
            return -ENODEV;
        }
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
        return store(p, &out, sizeof(out));
    }

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
    if (pgid == 0) {
        pgid = t->pid;
    }
    if (pgid != t->pid) {
        for (i = 0; (m = task_nth(i)) != 0; i++) {
            if (m->pgid == pgid && m->state != TASK_ZOMBIE) {
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

    case __NR_fsync:
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
        si.mem_unit = (u32)PAGE_SIZE;
        si.procs = (u32)task_count();
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

    case __NR_spawn:
        return do_spawn(a1, (int)a2, a3, a4);

    case __NR_jobctl:
        return do_jobctl((int)a1, (int)a2, a3);

    case __NR_netctl:
        return do_netctl((int)a1, a2, a3);

    case __NR_socket:
        return sock_create((int)a1, (int)a2, (int)a3);

    case __NR_bind:
    case __NR_connect: {
        struct sockaddr_in sa;
        int err = fetch(&sa, a2, sizeof(sa));

        if (err < 0) {
            return err;
        }
        return nr == __NR_bind ? sock_bind((int)a1, &sa)
                               : sock_connect((int)a1, &sa);
    }

    case __NR_listen:
        return sock_listen((int)a1, (int)a2);

    case __NR_accept: {
        struct sockaddr_in sa;
        int fd = sock_accept((int)a1, &sa);

        if (fd < 0) {
            return fd;
        }
        if (a2) {
            int err = store(a2, &sa, sizeof(sa));

            if (err < 0) {
                fd_close(fd);
                return err;
            }
        }
        return fd;
    }

    case __NR_sendto: {
        struct sockaddr_in sa;
        u8 buf[512];
        u32 n = a3;
        int err;

        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        err = fetch(buf, a2, n);
        if (err < 0) {
            return err;
        }
        err = fetch(&sa, a4, sizeof(sa));
        if (err < 0) {
            return err;
        }
        return sock_sendto((int)a1, buf, n, &sa);
    }

    case __NR_recvfrom: {
        struct sockaddr_in sa;
        u8 buf[512];
        u32 n = a3;
        s32 got;
        int err;

        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        got = sock_recvfrom((int)a1, buf, n, &sa);
        if (got < 0) {
            return got;
        }
        err = store(a2, buf, (u32)got);
        if (err < 0) {
            return err;
        }
        if (a4) {
            err = store(a4, &sa, sizeof(sa));
            if (err < 0) {
                return err;
            }
        }
        return got;
    }

    case __NR_shutdown:
        return sock_shutdown((int)a1, (int)a2);

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
        return current->pid;

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
        int got = task_wait((int)a1, &status);

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
        return -ENOSYS;
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

/* ---------------------------------------------------------------- */
/* Wrappers                                                          */
/* ---------------------------------------------------------------- */

int sys_open(const char *path, int flags)
{
    return (int)syscall2(__NR_open, (u32)path, (u32)flags);
}

int sys_close(int fd)
{
    return (int)syscall1(__NR_close, (u32)fd);
}

s32 sys_read(int fd, void *buf, u32 len)
{
    return syscall3(__NR_read, (u32)fd, (u32)buf, len);
}

s32 sys_write(int fd, const void *buf, u32 len)
{
    return syscall3(__NR_write, (u32)fd, (u32)buf, len);
}

s32 sys_lseek(int fd, s32 offset, int whence)
{
    return syscall3(__NR_lseek, (u32)fd, (u32)offset, (u32)whence);
}

int sys_unlink(const char *path)
{
    return (int)syscall1(__NR_unlink, (u32)path);
}

int sys_rename(const char *from, const char *to)
{
    return (int)syscall2(__NR_rename, (u32)from, (u32)to);
}

int sys_stat(const char *path, void *st)
{
    return (int)syscall2(__NR_stat, (u32)path, (u32)st);
}

int sys_getdents(int index, void *dirent)
{
    return (int)syscall2(__NR_getdents, (u32)index, (u32)dirent);
}

int sys_statfs(void *sfs)
{
    return (int)syscall1(__NR_statfs, (u32)sfs);
}

int sys_fsync(int fd)
{
    return (int)syscall1(__NR_fsync, (u32)fd);
}

int sys_sync(void)
{
    return (int)syscall0(__NR_sync);
}

int sys_sysinfo(struct sysinfo *si)
{
    return (int)syscall1(__NR_sysinfo, (u32)si);
}

int sys_uname(struct utsname *u)
{
    return (int)syscall1(__NR_uname, (u32)u);
}

int sys_ioctl(int fd, u32 request, u32 arg)
{
    return (int)syscall3(__NR_ioctl, (u32)fd, request, arg);
}

int sys_spawn(const char *path, int argc, char **argv, char **envp)
{
    return (int)syscall4(__NR_spawn, (u32)path, (u32)argc, (u32)argv,
                         (u32)envp);
}

int sys_jobctl(int cmd, int arg, void *p)
{
    return (int)syscall3(__NR_jobctl, (u32)cmd, (u32)arg, (u32)p);
}

int sys_mkdir(const char *path)
{
    return (int)syscall1(__NR_mkdir, (u32)path);
}

int sys_rmdir(const char *path)
{
    return (int)syscall1(__NR_rmdir, (u32)path);
}

int sys_chdir(const char *path)
{
    return (int)syscall1(__NR_chdir, (u32)path);
}

int sys_getcwd(char *buf, u32 len)
{
    return (int)syscall2(__NR_getcwd, (u32)buf, len);
}

int sys_waitpid(int pid, int *status)
{
    return (int)syscall2(__NR_waitpid, (u32)pid, (u32)status);
}

int sys_kill(int pid, int sig)
{
    return (int)syscall2(__NR_kill, (u32)pid, (u32)sig);
}

int sys_getpid(void)
{
    return (int)syscall0(__NR_getpid);
}

int sys_setpgid(int pid, int pgid)
{
    return (int)syscall2(__NR_setpgid, (u32)pid, (u32)pgid);
}

int sys_pipe(int fds[2])
{
    return (int)syscall1(__NR_pipe, (u32)fds);
}

int sys_dup(int fd)
{
    return (int)syscall1(__NR_dup, (u32)fd);
}

int sys_dup2(int oldfd, int newfd)
{
    return (int)syscall2(__NR_dup2, (u32)oldfd, (u32)newfd);
}

int sys_fcntl(int fd, int cmd, u32 arg)
{
    return (int)syscall3(__NR_fcntl, (u32)fd, (u32)cmd, arg);
}

void sys_yield(void)
{
    syscall0(__NR_sched_yield);
}

int sys_netctl(int cmd, u32 arg, void *p)
{
    return (int)syscall3(__NR_netctl, (u32)cmd, arg, (u32)p);
}

int sys_socket(int domain, int type, int protocol)
{
    return (int)syscall3(__NR_socket, (u32)domain, (u32)type, (u32)protocol);
}

int sys_connect(int fd, const struct sockaddr_in *addr)
{
    return (int)syscall2(__NR_connect, (u32)fd, (u32)addr);
}

u32 sys_times(void)
{
    return (u32)syscall0(__NR_times);
}

int sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
    return (int)syscall2(__NR_nanosleep, (u32)req, (u32)rem);
}

void sys_exit(int status)
{
    syscall1(__NR_exit, (u32)status);
}

time_t sys_time(time_t *t)
{
    return (time_t)syscall1(__NR_time, (u32)t);
}

int sys_stime(const time_t *t)
{
    return (int)syscall1(__NR_stime, (u32)t);
}

void sys_reboot(int cmd)
{
    syscall1(__NR_reboot, (u32)cmd);
    halt();                     /* the call does not return */
}
