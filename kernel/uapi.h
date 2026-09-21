/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * uapi.h - the types and constants that cross the system call boundary.
 *
 * Everything here is part of the contract between the kernel and the
 * programs that call it, which is why it is separate from vfs.h: a
 * program needs `struct stat` and O_CREAT, and has no business seeing
 * `struct fs_type` or the descriptor table.
 *
 * Linux keeps the same split for the same reason, under the same name.
 * When programs are separately compiled, this is the header they get and
 * the only one.
 */
#ifndef UAPI_H
#define UAPI_H

/*
 * The types, and nothing else. A program that includes this gets the
 * system call ABI without the kernel's internals or the machine's
 * register map coming with it.
 */
#include "types.h"

#define NAME_MAX      12        /* "12345678.123" without the NUL     */
#define PATH_MAX      64
#define OPEN_MAX      8         /* file descriptors per system        */

/*
 * open() flags. The access mode is the low two bits, the way POSIX has
 * it -- which means O_RDONLY is zero and testing for it with & does not
 * work. Use (flags & O_ACCMODE).
 */
#define O_RDONLY      0x0000
#define O_WRONLY      0x0001
#define O_RDWR        0x0002
#define O_ACCMODE     0x0003
#define O_CREAT       0x0040
#define O_TRUNC       0x0200
#define O_APPEND      0x0400

/*
 * ioctl requests. FIONREAD is Linux's, with Linux's number, and it is
 * the one a program needs to ask "has a key been pressed" without
 * blocking on a read that may never return.
 */
#define FIONREAD      0x541B

/*
 * Where console output goes, and where its input comes from.
 *
 * Local to this system, and numbered in the 0x54F0 block to say so --
 * Linux's own TIOC numbers stop well below it. There is no Linux
 * equivalent because there is no Linux equivalent idea: its console is
 * picked at boot with console= and listed in /proc/consoles, neither of
 * which is something a program asks a descriptor about. This machine
 * genuinely has a screen and a serial line at once, and a keyboard and
 * a serial line at once, so something has to be able to ask -- and the
 * terminal is the descriptor that has the answer.
 *
 * TIOCGCONS fills in one entry by index and returns -ENOENT past the
 * end, so a caller walks it from 0 without asking how many there are.
 */
#define TIOCGCONS     0x54F0    /* struct console_info *, in and out  */
#define TIOCSCONS     0x54F1    /* struct console_set *, in           */

#define CONS_SINK     1         /* somewhere output goes              */
#define CONS_SOURCE   2         /* somewhere input comes from         */

struct console_info {
    int  which;                 /* in:  CONS_SINK or CONS_SOURCE      */
    int  index;                 /* in:  which one, from 0             */
    int  enabled;               /* out: always 1 for a source, which
                                 *      cannot be turned off          */
    char name[16];              /* out                                */
};

struct console_set {
    char name[16];
    int  on;
};

/*
 * Terminal settings, Linux's numbers and Linux's structure.
 *
 * The reason these exist is that the line editor belongs in the shell,
 * not in the kernel. bash does not ask the kernel for history or for
 * ctrl-A; it turns canonical mode off and does the editing itself, in
 * readline, in userspace. Doing the same here keeps the kernel's
 * terminal small -- it assembles a line, or it hands over characters as
 * they arrive -- and means the editor moves across the privilege
 * boundary unchanged when programs stop running as the kernel.
 *
 * Not every flag is honoured. The ones that are, are the ones that
 * change behaviour: ICANON, ECHO and ISIG on the way in, ICRNL and
 * ONLCR on the translations, and the c_cc entries for the characters
 * that mean something. The rest are accepted and stored so that a
 * program can read back what it wrote, because a get/set pair that
 * quietly drops fields is worse than one that refuses them.
 */
#define TCGETS        0x5401
#define TCSETS        0x5402
#define TCSETSW       0x5403    /* no output queue to drain: same as TCSETS */
#define TCSETSF       0x5404    /* flushes input as well                   */

#define NCCS          19

struct termios {
    u32 c_iflag;
    u32 c_oflag;
    u32 c_cflag;
    u32 c_lflag;
    u8  c_line;
    u8  c_cc[NCCS];
};

/* c_iflag */
#define ICRNL         0x0100    /* carriage return arrives as newline */

/* c_oflag */
#define OPOST         0x0001    /* do output processing at all        */
#define ONLCR         0x0004    /* newline goes out as CR LF          */

/* c_lflag */
#define ISIG          0x0001    /* INTR and SUSP raise signals        */
#define ICANON        0x0002    /* assemble whole lines               */
#define ECHO          0x0008    /* echo what arrives                  */

/* c_cc indices, Linux's order. */
#define VINTR         0
#define VQUIT         1
#define VERASE        2
#define VKILL         3
#define VEOF          4
#define VTIME         5
#define VMIN          6
#define VSUSP         10

/*
 * Framebuffer ioctls, on /dev/fb0.
 *
 * Drawing through ioctl rather than through system calls of its own: a
 * framebuffer is a device, the device model already carries it, and
 * putting a dozen graphics calls in the system call table would tie the
 * kernel's ABI to one kind of hardware. Linux does its framebuffer
 * control this way for the same reason.
 *
 * Numbers are in Linux's framebuffer range but are not Linux's calls --
 * Linux has no "draw a line" ioctl, because it expects a program to map
 * the memory and draw for itself. Mapping needs an MMU, which is off.
 */
#define FBIO_GETINFO  0x4600    /* struct fb_info out                 */
#define FBIO_SETMODE  0x4601    /* struct fb_mode in                  */
#define FBIO_POINT    0x4602    /* struct fb_point in                 */
#define FBIO_LINE     0x4603    /* struct fb_line in                  */
#define FBIO_RECT     0x4604    /* struct fb_rect in                  */
#define FBIO_CLEAR    0x4605    /* colour, by value                   */
#define FBIO_FLIP     0x4606    /* show the drawn buffer              */
#define FBIO_SYNC     0x4607    /* wait for the blitter               */
#define FBIO_PALETTE  0x4608    /* struct fb_palette in               */
#define FBIO_COPY     0x4609    /* struct fb_copy in                  */
#define FBIO_DOUBLE   0x460A    /* double buffering on/off, by value  */

struct fb_info {
    u32 width;
    u32 height;
    u32 bpp;
    u32 pitch;
    char name[16];
};

struct fb_mode {
    u32 width;
    u32 height;
    u32 bpp;
};

struct fb_point {
    s32 x, y;
    u32 colour;
};

struct fb_line {
    s32 x0, y0, x1, y1;
    u32 colour;
};

struct fb_rect {
    s32 x, y;
    s32 w, h;
    u32 colour;
    u32 filled;
};

struct fb_copy {
    s32 sx, sy;                 /* where from                         */
    s32 dx, dy;                 /* where to                           */
    s32 w, h;
};

struct fb_palette {
    u32 index;
    u32 rgb;                    /* 0x00RRGGBB */
};

/* lseek() origins */
#define SEEK_SET      0
#define SEEK_CUR      1
#define SEEK_END      2

/* st_mode, as far as this kernel has a use for it */
#define S_IFMT        0170000
#define S_IFREG       0100000
#define S_IFDIR       0040000
#define S_IFCHR       0020000
#define S_IFBLK       0060000
#define S_IRUSR       0000400
#define S_IWUSR       0000200

#define S_ISREG(m)    (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)    (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)    (((m) & S_IFMT) == S_IFCHR)

struct stat {
    u32    st_mode;
    u32    st_size;
    time_t st_mtime;
    u32    st_blocks;
};

struct dirent {
    char   d_name[NAME_MAX + 1];
    u32    d_size;
    u32    d_mode;
    time_t d_mtime;
};

struct statfs {
    u32  f_bsize;               /* cluster size                       */
    u32  f_blocks;              /* total clusters                     */
    u32  f_bfree;               /* free clusters                      */
    char f_label[12];
    const char *f_type;         /* "fat16"                            */
};

/* ---------------------------------------------------------------- */
/* System call numbers                                               */
/*                                                                    */
/* Part of the ABI, so they live here rather than in the kernel's own */
/* header: a program needs them and needs nothing else from it.       */
/* ---------------------------------------------------------------- */

#define __NR_exit        1
#define __NR_read        3
#define __NR_write       4
#define __NR_open        5
#define __NR_close       6
#define __NR_unlink     10
#define __NR_time       13
#define __NR_lseek      19
#define __NR_stime      25
#define __NR_rename     38
#define __NR_ioctl      54
#define __NR_reboot     88
#define __NR_statfs     99
#define __NR_stat      106
#define __NR_fsync     118
#define __NR_sysinfo   116
#define __NR_uname     122
#define __NR_getdents  141
#define __NR_sync      166      /* Linux has 36; 166 keeps it clear of
                                 * this table's own use of 36..38      */

/*
 * Above 400 are calls Linux does not have, numbered well clear of it so
 * that nothing here can be mistaken for the real thing.
 *
 * spawn() is not execve(). execve replaces the calling process, and
 * there are no processes here to replace: this loads a program, runs it,
 * and returns its exit status. When there are processes it becomes
 * fork + execve + waitpid, the caller keeps the same shape, and this
 * number goes away.
 */
/*
 * Sockets, with Linux's own i386 numbers -- the direct calls rather than
 * the old socketcall(102) multiplexer, which existed because i386 ran
 * out of argument registers and this machine has not.
 *
 * A socket IS a file descriptor here, as it is on any Unix, so read(),
 * write() and close() work on one and there is no send()/recv() pair
 * for the stream case. That is not economy: it is what lets a program
 * be pointed at a socket instead of a file without knowing.
 */
#define __NR_socket    359
#define __NR_bind      361
#define __NR_connect   362
#define __NR_listen    363
#define __NR_accept    364
#define __NR_sendto    369
#define __NR_recvfrom  371
#define __NR_shutdown  373

#define AF_INET         2

#define SOCK_STREAM     1
#define SOCK_DGRAM      2

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

/*
 * The BSD address structure, unchanged.
 *
 * sin_port and sin_addr are in NETWORK byte order, which on this machine
 * is also host order -- so htons() and ntohl() are the identity here and
 * compile to nothing. A program should still call them: the day this
 * code is read on a little-endian machine the habit is what makes it
 * portable, and the cost of the habit is zero.
 */
struct sockaddr_in {
    u16 sin_family;
    u16 sin_port;
    u32 sin_addr;
    u8  sin_zero[8];
};

#define INADDR_ANY      0x00000000UL

#define __NR_spawn     400
#define __NR_jobctl    401
#define __NR_netctl    402

/*
 * What spawn() returns when the program was stopped by ctrl-Z rather
 * than finishing. It is still in the job table, still holding the
 * program area, and `fg` will resume it.
 *
 * Not an errno, because nothing went wrong, and not a plausible exit
 * status either -- an exit status is a byte on any system that has
 * waitpid, so nothing can legitimately return this.
 */
#define SPAWN_STOPPED  0x7fffffff

/*
 * jobctl() - ask about, or act on, the shell's jobs.
 *
 * Local to this system, like spawn, and for the same reason: Linux does
 * this with fork, waitpid, kill and tcsetpgrp, none of which mean
 * anything without processes. One call with a command keeps the
 * placeholder small and obvious rather than spreading four fictional
 * Linux numbers through the table. It goes away in the same change that
 * makes spawn into fork and execve.
 */
#define JOBCTL_INFO    0        /* a2 = struct job_info *, by index  */
#define JOBCTL_FG      1        /* resume a stopped job              */
#define JOBCTL_BG      2        /* run one in the background         */
#define JOBCTL_QUEUE   3        /* a2 = command line; create JOB_NEW */
#define JOBCTL_REAP    4        /* forget the finished ones          */
#define JOBCTL_DROP    5        /* forget one by id                  */

/*
 * netctl() - ask about, or configure, the network interface.
 *
 * Local, like spawn and jobctl. Linux does this with ioctls on a socket
 * -- SIOCGIFADDR and friends -- which needs sockets to exist first, and
 * they do not yet. When they do, this becomes those ioctls and the
 * shell's commands keep their shape.
 */
#define NETCTL_INFO    0        /* p = struct netinfo *               */
#define NETCTL_SETADDR 1        /* p = struct netaddr *               */
#define NETCTL_ARPING  2        /* arg = IPv4 address, host order     */
#define NETCTL_ARP     3        /* arg = index, p = struct arpinfo *  */
#define NETCTL_PING    6        /* arg = address; p = u32 *rtt_ms     */
#define NETCTL_DHCP    7        /* p = struct netaddr * (filled in)   */
#define NETCTL_UP      4
#define NETCTL_DOWN    5

struct netinfo {
    char name[8];
    u8   mac[6];
    u8   pad[2];
    u32  ip;
    u32  netmask;
    u32  gateway;
    u32  up;
    u32  rx_packets;
    u32  tx_packets;
    u32  rx_dropped;
    u32  tx_errors;
};

struct netaddr {
    u32 ip;
    u32 netmask;
    u32 gateway;
};

struct arpinfo {
    u32 ip;
    u8  mac[6];
    u8  pad[2];
    u32 age_ms;
};

/* Job states, as JOBCTL_INFO reports them. */
#define JOB_S_NEW      1
#define JOB_S_RUNNING  2
#define JOB_S_STOPPED  3
#define JOB_S_DONE     4

struct job_info {
    int  id;
    int  state;
    int  background;
    int  status;
    int  signalled;
    char cmd[128];
};

/*
 * times() here returns ticks since boot and takes no struct tms: there
 * are no processes to account time to. The number is Linux's; the
 * meaning is the subset of it that makes sense.
 */
#define __NR_times      43
#define __NR_nanosleep 162

/*
 * How many of those ticks there are in a second.
 *
 * Part of the ABI rather than a kernel constant, because times() returns
 * ticks and a number of ticks means nothing without it. Linux answers
 * the same question through sysconf(_SC_CLK_TCK); there is no sysconf
 * here, so it is a number in the header a program already includes.
 */
#define HZ             100

/* Standard descriptors, bound to the console at startup. */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/*
 * nanosleep()'s argument. The kernel's tick is 10 ms, so anything finer
 * than that rounds up to one tick -- a sleep that returns early is a
 * bug waiting to happen and one tick late is nothing.
 */
struct timespec {
    u32 tv_sec;
    u32 tv_nsec;
};

/*
 * What sysinfo() fills in.
 *
 * Linux's call, with Linux's number and a cut-down version of its
 * structure: the fields that mean something on a machine with no swap
 * and no load average are there and the rest are not. `mem_unit` is
 * Linux's way of reporting sizes in something other than bytes, and it
 * is the page size here -- which is the unit the allocator actually
 * works in, so reporting anything else would be arithmetic performed in
 * order to be undone.
 */
struct sysinfo {
    u32 uptime;                 /* seconds since boot            */
    u32 totalram;               /* in mem_unit                   */
    u32 freeram;
    u32 procs;                  /* jobs that exist               */
    u32 mem_unit;               /* bytes per unit: the page size */
};

/* What uname() fills in. */
struct utsname {
    char sysname[16];
    char release[16];
    char machine[16];
    char version[32];
};

/*
 * Signals.
 *
 * Only the ones the terminal can raise, with Linux's numbers. There is
 * no sigaction() and no handler: a signal here is something the kernel
 * does TO a job, not something a program catches. That is enough for
 * what a terminal needs -- interrupt, stop, continue -- and it is the
 * part that has to exist before ctrl-C can mean anything.
 *
 * A program killed by one exits with 128 + the number, which is the
 * convention every Unix shell reports and the one `echo $?` would show.
 */
#define SIGINT          2
#define SIGILL          4
#define SIGFPE          8
#define SIGSEGV        11
#define SIGKILL         9
#define SIGTERM         15
#define SIGCONT         18
#define SIGTSTP         20      /* ctrl-Z */

/* reboot() commands, Linux's magic values cut down to what is useful. */
#define RB_HALT_SYSTEM  0xcdef0123
#define RB_AUTOBOOT     0x01234567
#define RB_POWER_OFF    0x4321fedc

#endif /* UAPI_H */
