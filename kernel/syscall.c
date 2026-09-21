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
#include "job.h"
#include "tty.h"
#include "timer.h"
#include "dev.h"
#include "console.h"
#include "uaccess.h"
#include "pmm.h"
#include "net.h"
#include "errno.h"
#include "string.h"

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
    }
    return total;
}

/* ---------------------------------------------------------------- */
/* The implementations                                               */
/* ---------------------------------------------------------------- */

static time_t clock_now(void)
{
    struct rtcdev *r = dev_rtc();
    time_t now = 0;

    if (r && r->get(r, &now) == 0) {
        return now;
    }
    return 0;
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
    struct rtcdev *r = dev_rtc();

    if (!t) {
        return -EINVAL;
    }
    if (!r || !r->set) {
        return -ENODEV;
    }
    return r->set(r, *t);
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

static int do_spawn(u32 upath, int argc, u32 uargv)
{
    char path[PATH_MAX];
    char argstore[EXEC_MAX_ARGS][SPAWN_ARG_MAX];
    char *argv[EXEC_MAX_ARGS];
    u32 uptr[EXEC_MAX_ARGS];
    int i, err;

    if (argc < 0 || argc > EXEC_MAX_ARGS) {
        return -E2BIG;
    }
    err = fetch_str(path, upath, sizeof(path));
    if (err < 0) {
        return err;
    }

    if (!from_program()) {
        return exec_spawn(path, argc, (char **)uargv);
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
    return exec_spawn(path, argc, argv);
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

/* --- jobs ---------------------------------------------------------- */

static int info_state(int state)
{
    switch (state) {
    case JOB_NEW:     return JOB_S_NEW;
    case JOB_RUNNING: return JOB_S_RUNNING;
    case JOB_STOPPED: return JOB_S_STOPPED;
    default:          return JOB_S_DONE;
    }
}

static int do_jobctl(int cmd, int arg, u32 p)
{
    struct job *j;

    switch (cmd) {
    case JOBCTL_INFO: {
        struct job_info out;

        j = job_nth(arg);
        if (!j) {
            return -ENOENT;
        }
        memset(&out, 0, sizeof(out));
        out.id = j->id;
        out.state = info_state(j->state);
        out.background = j->background;
        out.status = j->status;
        out.signalled = j->signalled;
        strncpy(out.cmd, j->cmd, sizeof(out.cmd) - 1);
        out.cmd[sizeof(out.cmd) - 1] = '\0';
        return store(p, &out, sizeof(out));
    }

    case JOBCTL_FG:
        /*
         * Only a stopped job is resumed here. One that was queued with &
         * has never run, so there is no context to return into -- the
         * shell still has its command line and starts it the ordinary
         * way, which is the same thing with less machinery.
         */
        return exec_continue(arg);

    case JOBCTL_BG:
        j = job_get(arg);
        if (!j) {
            return -ENOENT;
        }
        /*
         * Running in the background means running while the shell also
         * runs, and nothing here can do two things at once yet. The job
         * keeps its place in the table and stays resumable with fg; what
         * is missing is a scheduler, and saying so is more use than
         * quietly running it in the foreground instead.
         */
        j->background = 1;
        return -ENOSYS;

    case JOBCTL_QUEUE: {
        char cmd[JOB_CMD_MAX];
        int err = fetch_str(cmd, p, sizeof(cmd));

        if (err < 0) {
            return err;
        }
        return job_create(cmd, 1);
    }

    case JOBCTL_REAP:
        job_reap();
        return 0;

    case JOBCTL_DROP:
        j = job_get(arg);
        if (!j) {
            return -ENOENT;
        }
        if (j->state == JOB_RUNNING || j->state == JOB_STOPPED) {
            return -EBUSY;
        }
        j->state = JOB_FREE;
        job_reap();
        return 0;

    default:
        return -EINVAL;
    }
}

/*
 * The table.
 *
 * Unimplemented numbers return -ENOSYS rather than doing something
 * approximate, because a call that silently does nothing is far harder
 * to find than one that says it does not exist.
 */
static s32 do_syscall(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    (void)a5;

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
        struct job *j;
        int i;

        memset(&si, 0, sizeof(si));
        si.uptime = timer_jiffies() / HZ;
        si.totalram = pmm_total();
        si.freeram = pmm_available();
        si.mem_unit = (u32)PAGE_SIZE;
        for (i = 0; (j = job_nth(i)) != 0; i++) {
            si.procs++;
        }
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
        return do_spawn(a1, (int)a2, a3);

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

    case __NR_times:
        return (s32)timer_jiffies();

    case __NR_nanosleep: {
        struct timespec ts;
        const struct timespec *req = &ts;
        u32 ms;
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
        if (a2) {
            struct timespec rem;

            /* Nothing interrupts a sleep yet -- no signals, one program
             * -- so there is never any remaining. Zeroed rather than
             * left alone, because a caller checking it deserves a
             * defined answer. */
            rem.tv_sec = 0;
            rem.tv_nsec = 0;
            err = store(a2, &rem, sizeof(rem));
            if (err < 0) {
                return err;
            }
        }
        return timer_sleep_ms(ms);
    }

    case __NR_exit:
        /* Returns only if nothing was spawned -- a program's exit()
         * unwinds all the way back into exec_spawn() and never comes
         * back here. */
        return exec_exit((int)a1);

    default:
        return -ENOSYS;
    }
}

/* ---------------------------------------------------------------- */
/* How deep in the kernel we are                                     */
/*                                                                    */
/* Counted because ctrl-C has to know. Killing a program means        */
/* throwing its whole stack away, and doing that from the middle of   */
/* a directory update leaves the directory half written. So a signal  */
/* raised while the kernel is working waits for the boundary, which   */
/* is a few instructions away, and one raised while the program is    */
/* running its own code is delivered by the timer tick straight away. */
/*                                                                    */
/* A count rather than a flag: the shell calls system calls too, and  */
/* spawn runs a whole program from inside one.                        */
/* ---------------------------------------------------------------- */

static volatile int depth;

int syscall_in_kernel(void)
{
    return depth != 0;
}

/*
 * THE COUNT BELONGS TO THE PROGRAM, NOT TO THE MACHINE.
 *
 * A program is started from inside a system call -- the shell's spawn --
 * so the count is already 1 when the program begins. Every system call
 * the program then makes goes 1 to 2 and back to 1, and never reaches
 * zero: its boundaries are invisible, the tick thinks the kernel is
 * always busy, and neither ctrl-C nor ctrl-Z can ever be delivered.
 *
 * So entering a program swaps the count to zero and leaving it swaps the
 * caller's back. A stopped job keeps its own count with it, because it
 * is stopped in the middle of a system call and has to come back to
 * exactly that depth.
 *
 * This also replaces resetting the count after a program is killed: a
 * program that jumped out without returning left the count wherever it
 * was, and the swap on the way out puts back the right value rather than
 * guessing at zero.
 */
int syscall_depth_swap(int d)
{
    int old = depth;

    depth = d;
    return old;
}

int syscall_depth(void)
{
    return depth;
}

s32 syscall_dispatch(u32 nr, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5)
{
    s32 r;

    depth++;
    r = do_syscall(nr, a1, a2, a3, a4, a5);
    depth--;

    /*
     * The boundary. The kernel has finished whatever it was doing and is
     * about to hand control back, which makes this the one place a
     * program can be killed or stopped without leaving anything half
     * done. Note it happens on the way OUT: a read that was interrupted
     * has already returned EINTR, and the program never sees it.
     */
    if (depth == 0) {
        job_deliver(JOB_AT_SYSCALL);
    }
    return r;
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

int sys_spawn(const char *path, int argc, char **argv)
{
    return (int)syscall3(__NR_spawn, (u32)path, (u32)argc, (u32)argv);
}

int sys_jobctl(int cmd, int arg, void *p)
{
    return (int)syscall3(__NR_jobctl, (u32)cmd, (u32)arg, (u32)p);
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
