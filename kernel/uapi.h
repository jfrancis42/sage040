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
#define OPEN_MAX      32        /* file descriptors per TASK           */

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
#define O_NONBLOCK    0x0800    /* reads and writes that would wait fail
                                 * with EAGAIN instead                    */
#define O_CLOEXEC     0x80000   /* the new descriptor is FD_CLOEXEC       */
#define O_EXCL        0x0080    /* with O_CREAT: fail if it exists        */
#define O_DIRECTORY   0x4000    /* m68k's value, not the generic 0x10000 */
#define O_NOFOLLOW    0x8000    /* accepted: there are no symlinks        */
#define O_LARGEFILE   0x20000   /* accepted: no file is over 4 GB         */

/*
 * ioctl requests. FIONREAD is Linux's, with Linux's number, and it is
 * the one a program needs to ask "has a key been pressed" without
 * blocking on a read that may never return.
 */
#define FIONREAD      0x541B
#define FIONBIO       0x5421    /* set or clear O_NONBLOCK, from an int  */

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

/*
 * termios2: the same, with the speeds as numbers rather than as bits in
 * c_cflag. Linux's, and the form picolibc asks for -- its isatty() is a
 * TCGETS2. The speeds are reported and not set: this terminal is a
 * console, and the serial line's rate is the emulator's business.
 */
struct termios2 {
    u32 c_iflag;
    u32 c_oflag;
    u32 c_cflag;
    u32 c_lflag;
    u8  c_line;
    u8  c_cc[NCCS];
    u32 c_ispeed;
    u32 c_ospeed;
};

#define TCGETS2       0x802C542A
#define TCSETS2       0x402C542B
#define TCSETSW2      0x402C542C
#define TCSETSF2      0x402C542D

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

/* On /dev/fbcon: draw the whole console again from its character
 * buffer. Numbered where TIOCGCONS is, past anything Linux uses. */
#define FBCON_REDRAW  0x46F0

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
#define S_IFIFO       0010000
#define S_IFSOCK      0140000
#define S_IRUSR       0000400
#define S_IWUSR       0000200

#define S_ISREG(m)    (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)    (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)    (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m)   (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)   (((m) & S_IFMT) == S_IFSOCK)

/* access() modes, Linux's values. */
#define F_OK          0
#define X_OK          1
#define W_OK          2
#define R_OK          4

/*
 * NOT Linux's struct stat, although stat (106) and fstat (108) are
 * Linux's numbers. These are this system's own, used by lib/ulib; a C
 * library uses statx (379), which IS Linux's -- see struct statx below.
 * The same goes for getdents (141) against getdents64 (220).
 */
struct stat {
    u32    st_mode;
    u32    st_size;
    time_t st_mtime;
    u32    st_blocks;
    u32    st_ino;              /* see ino_for() in fs/fat16.c         */
};

struct dirent {
    char   d_name[NAME_MAX + 1];
    u32    d_size;
    u32    d_mode;
    time_t d_mtime;
    u32    d_ino;
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
#define __NR_mkdir      39
#define __NR_rmdir      40
#define __NR_chdir      12
#define __NR_getcwd    183
#define __NR_rename     38
#define __NR_ioctl      54
#define __NR_reboot     88
#define __NR_statfs     99
#define __NR_stat      106
#define __NR_fstat     108      /* describe an open descriptor        */
#define __NR_access     33      /* answered from stat; see vfs.c      */
#define __NR_dup        41
#define __NR_dup2       63
#define __NR_fsync     118
#define __NR_sysinfo   116
#define __NR_uname     122
#define __NR_getdents  141
#define __NR_sync       36

/*
 * The program break. Linux's number and Linux's convention, which is
 * NOT the usual one: brk(addr) returns the new break on success and the
 * old one, unchanged, on failure -- never an errno. brk(0) asks where
 * it is. The library's brk() and sbrk() turn that into -1 and ENOMEM.
 */
#define __NR_brk        45

/*
 * Pipes, descriptors and process groups. Linux's numbers.
 *
 * pipe() fills in two descriptors, [0] to read and [1] to write. A pipe
 * holds PIPE_SIZE bytes; a write of up to PIPE_BUF bytes is not split
 * up by other writers' data. Reading an empty pipe with no writers left
 * is end of file; writing one with no readers left raises SIGPIPE and
 * fails with EPIPE.
 *
 * A PROCESS GROUP is what a terminal's ctrl-C is aimed at: every task in
 * the foreground group gets it, which is how it reaches every command
 * in a pipeline. A task starts in its parent's group; setpgid() moves
 * it. A task that reads the terminal while not in the foreground group
 * is sent SIGTTIN, which stops it until `fg` -- the read then carries
 * on, restarted, as if nothing had happened.
 */
#define __NR_pipe           42

/*
 * Processes, the Unix way. Linux's numbers.
 *
 * fork() copies the address space eagerly -- every page, now -- because
 * there is no copy-on-write yet (that wants the page-fault machinery of
 * progress.md task 21). execve() replaces the calling program; spawn()
 * (400) is still here and is still fork and exec in one.
 *
 * waitpid's status is Linux's encoding, read with the W* macros below.
 * pid > 0 is that child, -1 any child, 0 any child in the caller's
 * group, and -N any child in group N.
 */
#define __NR_fork            2
#define __NR_execve         11

#define WNOHANG         0x00000001
#define WUNTRACED       0x00000002
#define WCONTINUED      0x00000008

#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSIGNALED(s)  (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WIFCONTINUED(s) ((s) == 0xffff)
#define __NR_fcntl          55
#define __NR_setpgid        57
#define __NR_getppid        64
#define __NR_getpgrp        65
#define __NR_getpgid       132

#define PIPE_SIZE       4096
#define PIPE_BUF        4096

#define F_DUPFD         0       /* lowest free descriptor >= arg      */
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4       /* only O_NONBLOCK and O_APPEND change */
#define FD_CLOEXEC      1       /* not given to a program spawn()ed    */

#define TIOCGPGRP       0x540F  /* the terminal's foreground group     */
#define TIOCSPGRP       0x5410

/*
 * The terminal's size, in Linux's numbers and layout. What TIOCGWINSZ
 * reports is the smallest of the console's enabled outputs, since a
 * full-screen program has to fit on all of them; TIOCSWINSZ sets the
 * size of the one output that cannot report its own -- the serial line,
 * 24x80 until told otherwise. A change to what TIOCGWINSZ would report
 * sends SIGWINCH to the foreground group.
 */
#define TIOCGWINSZ      0x5413
#define TIOCSWINSZ      0x5414

struct winsize {
    u16 ws_row;
    u16 ws_col;
    u16 ws_xpixel;
    u16 ws_ypixel;
};

/*
 * Waiting on several descriptors. Linux/m68k's numbers and shapes:
 *
 *   poll        168  (struct pollfd *, count, timeout in ms; -1 waits)
 *   _newselect  142  (nfds, readfds, writefds, exceptfds, timeval *)
 *   select       82  ONE argument, a pointer to struct sel_arg_struct --
 *                    the old interface, as number 82 is on Linux/m68k
 *
 * select() writes back the time left, as Linux's does. Both are
 * interrupted by a signal: -EINTR after a handler, a restart if none
 * ran (which starts the timeout again rather than resuming it).
 *
 * A terminal in canonical mode is readable when a CHARACTER is waiting,
 * not when a whole line is: the line is assembled inside read(), so
 * nothing outside it knows where one ends. In raw mode, which is what a
 * program that polls a terminal uses, readable means exactly that.
 */
struct timeval {
    s32 tv_sec;
    s32 tv_usec;
};

/*
 * Timers and the clock. Linux's numbers, which m68k shares with i386
 * for everything this old.
 *
 * ITIMER_REAL counts wall time and raises SIGALRM; ITIMER_VIRTUAL
 * counts the time the process runs in USER mode and raises SIGVTALRM;
 * ITIMER_PROF counts user and system time and raises SIGPROF. All three
 * run at the resolution of the tick, 10 ms, and a value shorter than a
 * tick is rounded up to one rather than to nothing. alarm() is
 * ITIMER_REAL in whole seconds, and the two share one timer, as they do
 * on Linux.
 *
 * gettimeofday() and time() read the same clock, so they cannot
 * disagree; see timer.c. The timezone argument is accepted and ignored,
 * as it is almost everywhere.
 */
#define __NR_alarm          27
#define __NR_gettimeofday   78
#define __NR_settimeofday   79
#define __NR_setitimer     104
#define __NR_getitimer     105

#define ITIMER_REAL         0
#define ITIMER_VIRTUAL      1
#define ITIMER_PROF         2

struct itimerval {
    struct timeval it_interval;     /* reload value; zero: one shot */
    struct timeval it_value;        /* time left; zero: disarmed    */
};

/*
 * What times() fills in, in ticks (clock_t, HZ per second). The
 * children's figures are for children that have been waited for,
 * as POSIX says.
 */
struct tms {
    u32 tms_utime;
    u32 tms_stime;
    u32 tms_cutime;
    u32 tms_cstime;
};

#define __NR_select     82
#define __NR__newselect 142
#define __NR_poll      168

#define POLLIN          0x0001
#define POLLPRI         0x0002
#define POLLOUT         0x0004
#define POLLERR         0x0008
#define POLLHUP         0x0010
#define POLLNVAL        0x0020
#define POLLRDNORM      0x0040
#define POLLRDBAND      0x0080
#define POLLWRNORM      POLLOUT     /* m68k's definition */
#define POLLWRBAND      0x0100

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define FD_SETSIZE      1024
typedef struct {
    u32 fds_bits[FD_SETSIZE / 32];
} fd_set;

#define FD_ZERO(s)      do { u32 i_; for (i_ = 0; i_ < FD_SETSIZE / 32; i_++) \
                             (s)->fds_bits[i_] = 0; } while (0)
#define FD_SET(fd, s)   ((s)->fds_bits[(fd) / 32] |= (1UL << ((fd) % 32)))
#define FD_CLR(fd, s)   ((s)->fds_bits[(fd) / 32] &= ~(1UL << ((fd) % 32)))
#define FD_ISSET(fd, s) (((s)->fds_bits[(fd) / 32] >> ((fd) % 32)) & 1)

struct sel_arg_struct {
    u32 n;
    fd_set *inp;
    fd_set *outp;
    fd_set *exp;
    struct timeval *tvp;
};

/*
 * Mapping memory. The numbers AND the shapes are Linux/m68k's:
 *
 *   mmap2    192  six arguments in d1-d5 and a0, the offset in PAGES.
 *                 This is the one a program should use.
 *   mmap      90  ONE argument, a pointer to struct mmap_arg_struct,
 *                 the offset in bytes. The old interface, from before
 *                 there was a sixth register convention; kept because
 *                 it is what number 90 means on Linux/m68k.
 *
 * Addresses returned are always below 0x80000000, so a result can never
 * be mistaken for a negated errno.
 */
#define __NR_mmap       90
#define __NR_munmap     91
#define __NR_mprotect  125
#define __NR_mmap2     192

struct mmap_arg_struct {
    u32 addr;
    u32 len;
    u32 prot;
    u32 flags;
    u32 fd;
    u32 offset;                 /* bytes, and page aligned */
};

/*
 * PROT_EXEC is accepted and means nothing: the 68040 has no execute
 * permission bit, so anything readable can be executed. PROT_WRITE
 * without PROT_READ gives read-write, because there is no write-only
 * page either. Both are what Linux does on hardware that cannot tell.
 */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE 0x100000

#define MAP_FAILED      ((void *)-1)

/*
 * Above 400 are calls Linux does not have, numbered well clear of it so
 * that nothing here can be mistaken for the real thing.
 *
 * spawn() is not execve(). execve replaces the calling process; this
 * creates a new task and returns its PID. IT DOES NOT WAIT -- whether
 * to wait is the caller's decision, and that decision is the whole of
 * what `&` means: a foreground job is one the shell waits for with
 * waitpid(), a background job is one it does not.
 *
 * It is still fork and execve rolled into one call. Splitting them is
 * what would let a caller arrange its own descriptors in between, which
 * is what a shell needs for redirection -- so this number goes away the
 * day there are pipes.
 */
/*
 * Sockets, with Linux/m68k's numbers -- the direct calls rather than
 * the old socketcall(102) multiplexer, which m68k Linux also still has
 * and which this machine does not need.
 *
 * A socket IS a file descriptor here, as it is on any Unix, so read(),
 * write() and close() work on one and there is no send()/recv() pair
 * for the stream case. That is not economy: it is what lets a program
 * be pointed at a socket instead of a file without knowing.
 */
#define __NR_kill       37
#define __NR_waitpid     7
#define __NR_getpid     20
#define __NR_sched_yield 158

/*
 * Linux/m68k's numbers, from arch/m68k/kernel/syscalls/syscall.tbl.
 * They were i386's -- every one three too high, so `socket` sat where
 * m68k has `connect` -- which nothing here noticed, because both ends
 * came from this header. A C library built on the real table did not
 * share the mistake.
 */
#define __NR_socket      356
#define __NR_socketpair  357
#define __NR_bind        358
#define __NR_connect     359
#define __NR_listen      360
#define __NR_accept4     361    /* accept() is this with flags 0, in the
                                 * library                               */
#define __NR_getsockopt  362
#define __NR_setsockopt  363
#define __NR_getsockname 364
#define __NR_getpeername 365
#define __NR_sendto      366
#define __NR_sendmsg     367
#define __NR_recvfrom    368
#define __NR_recvmsg     369
#define __NR_shutdown    370

/*
 * The socket interface is Linux's now, signatures and all: a struct
 * sockaddr and a socklen_t beside it, and the flags arguments. It took
 * a struct sockaddr_in and no length until a C library was coming,
 * whose socket layer is written against the real thing.
 *
 * AF_UNIX exists as socketpair() only -- a connected pair, stream, the
 * same object as a pipe in each direction. There are no named Unix
 * sockets, because there is no file type on a FAT volume to be one.
 */
#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_LOCAL        AF_UNIX
#define AF_INET         2
#define PF_UNSPEC       AF_UNSPEC
#define PF_UNIX         AF_UNIX
#define PF_LOCAL        AF_UNIX
#define PF_INET         AF_INET

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_NONBLOCK   O_NONBLOCK      /* in socket()'s type, and accept4 */
#define SOCK_CLOEXEC    O_CLOEXEC

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define MSG_PEEK        0x0002
#define MSG_TRUNC       0x0020
#define MSG_DONTWAIT    0x0040
#define MSG_WAITALL     0x0100
#define MSG_NOSIGNAL    0x4000

/* setsockopt/getsockopt: the options that mean something here. */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_ACCEPTCONN   30
#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define TCP_NODELAY     1       /* always on: there is no Nagle to turn off */

typedef u32 socklen_t;

struct sockaddr {
    u16  sa_family;
    char sa_data[14];
};

/*
 * sin_port and sin_addr are in NETWORK byte order, which on this machine
 * is also host order -- so htons() and ntohl() are the identity here and
 * compile to nothing. A program should still call them: the habit is
 * what makes it portable, and it costs nothing.
 */
struct in_addr {
    u32 s_addr;
};

struct sockaddr_in {
    u16 sin_family;
    u16 sin_port;
    struct in_addr sin_addr;
    u8  sin_zero[8];
};

#define INADDR_ANY       0x00000000UL
#define INADDR_LOOPBACK  0x7f000001UL   /* 127.0.0.1 */
#define INADDR_BROADCAST 0xffffffffUL
#define INADDR_NONE      0xffffffffUL   /* inet_addr's "not an address" */

struct iovec {
    void *iov_base;
    u32   iov_len;
};

/* No ancillary data: msg_control is accepted, and msg_controllen comes
 * back 0 with MSG_CTRUNC set if any was asked for. */
#define MSG_CTRUNC      0x0008

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    u32           msg_iovlen;
    void         *msg_control;
    u32           msg_controllen;
    int           msg_flags;
};

/*
 * This system's own calls, numbered where Linux will not reach: they
 * were 400-402, which Linux/m68k assigns to msgsnd, msgrcv and msgctl,
 * so a ported program calling msgsnd would have spawned something.
 */
#define __NR_spawn     1000
#define __NR_jobctl    1001
#define __NR_netctl    1002

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
#define JOBCTL_ALL     6        /* every task, not just this one's    */

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
#define NETCTL_CONN    8        /* arg = index, p = struct conninfo * */
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

/* One TCP connection, as netstat sees it. */
struct conninfo {
    u32 local_ip;
    u32 remote_ip;
    u16 local_port;
    u16 remote_port;
    u32 state;
    u32 txq;                    /* bytes waiting to be acknowledged */
    u32 rxq;                    /* bytes waiting to be read         */
    u32 cwnd;
    u32 rtt_ms;
    char state_name[16];
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
#define JOB_S_BLOCKED  5

/* How much of a command line is kept, for `jobs` and for `fg`. */
#define JOB_CMD_MAX    128

struct job_info {
    int  id;                    /* the pid                            */
    int  state;
    int  background;
    int  status;
    int  signalled;
    int  ppid;
    char cmd[128];
};

/*
 * times() returns ticks since boot, and fills in a struct tms if it is
 * given one: this task's user and system time, and its waited-for
 * children's. It used to take no argument, from before there was
 * anything to account time to.
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
 * Signals, with Linux's numbers -- all 31 of them, because a ported
 * program names whichever it likes and must get the number it expects.
 *
 * A program killed by one exits with 128 + the number, which is the
 * convention every Unix shell reports and the one `echo $?` would show.
 */
#define SIGHUP           1
#define SIGINT           2
#define SIGQUIT          3
#define SIGILL           4
#define SIGTRAP          5
#define SIGABRT          6
#define SIGBUS           7
#define SIGFPE           8
#define SIGKILL          9
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGSTKFLT       16
#define SIGCHLD         17
#define SIGCONT         18
#define SIGSTOP         19
#define SIGTSTP         20      /* ctrl-Z */
#define SIGTTIN         21
#define SIGTTOU         22
#define SIGURG          23
#define SIGXCPU         24
#define SIGXFSZ         25
#define SIGVTALRM       26
#define SIGPROF         27
#define SIGWINCH        28
#define SIGIO           29
#define SIGPWR          30
#define SIGSYS          31
#define NSIG            32      /* one more than the highest */

/*
 * Catching them. The old, 32-bit-mask interface, with Linux/m68k's
 * numbers and its layout of struct sigaction -- handler, mask, flags,
 * restorer, in that order, which is m68k's and not i386's.
 *
 * A mask has signal N in bit N-1, as Linux's old_sigset_t does.
 *
 * sa_restorer is optional. With SA_RESTORER the handler returns there
 * -- lib/ulib supplies one, in crt0.s. Without it, the kernel writes
 * Linux/m68k's two-instruction trampoline into the signal frame, as
 * Linux/m68k always does. SA_SIGINFO handlers get Linux/m68k's rt frame
 * and always return through the kernel's trampoline; see the rt_ calls
 * further down.
 *
 * NOT SUPPORTED, and refused rather than half done: SA_ONSTACK (there
 * is no sigaltstack).
 */
#define __NR_pause          29
#define __NR_sigaction      67
#define __NR_sigsuspend     72  /* one argument: the mask to wait with */
#define __NR_sigpending     73
#define __NR_sigreturn     119
#define __NR_sigprocmask   126

typedef void (*sighandler_t)(int);
typedef u32 sigset_t;

#define SIG_DFL         ((sighandler_t)0)
#define SIG_IGN         ((sighandler_t)1)
#define SIG_ERR         ((sighandler_t)-1)

struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    u32          sa_flags;
    void       (*sa_restorer)(void);
};

#define SA_NOCLDSTOP    0x00000001
#define SA_SIGINFO      0x00000004      /* three arguments: see struct siginfo */
#define SA_RESTORER     0x04000000
#define SA_ONSTACK      0x08000000      /* refused: no sigaltstack */
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000

#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2

/*
 * What a handler finds on its stack, from the lowest address up:
 *
 *   return address   -> sa_restorer
 *   int sig
 *   int code         always 0 here
 *   struct sigcontext *
 *   struct sigcontext
 *
 * so a handler is an ordinary function of one argument. The context is
 * everything the interrupted code had -- every register, the mask, the
 * FPU -- and sigreturn puts it all back. Only the condition codes of
 * the saved sr are honoured on the way back; the rest of it is not the
 * program's to set.
 */
struct sigcontext {
    u32 sc_mask;
    u32 sc_usp;
    u32 sc_d[8];
    u32 sc_a[7];
    u16 sc_sr;
    u32 sc_pc;
    u16 sc_format;
    u32 sc_fpu[52];             /* fsave frame, fp0-fp7, fpcr/fpsr/fpiar */
} __attribute__((packed));

/* ---------------------------------------------------------------- */
/* The rest of Linux's interface, for a C library                    */
/*                                                                    */
/* Everything below is Linux/m68k's, layout and all, because picolibc */
/* (lib/libc) is built against Linux's own headers for m68k and makes */
/* these calls exactly as it would on Linux. None of it is used by    */
/* lib/ulib, which keeps this system's simpler calls above.           */
/* ---------------------------------------------------------------- */

#define __NR_chmod          15
#define __NR_setuid         23
#define __NR_getuid         24
#define __NR_setgid         46
#define __NR_getgid         47
#define __NR_geteuid        49
#define __NR_getegid        50
#define __NR_umask          60
#define __NR_setreuid       70
#define __NR_setregid       71
#define __NR_setrlimit      75
#define __NR_getrlimit      76
#define __NR_getgroups      80
#define __NR_setgroups      81
#define __NR_readlink       85
#define __NR_fstatfs       100
#define __NR_lstat         107  /* this system's struct stat, like stat */
#define __NR_wait4         114
#define __NR_clone         120
#define __NR_fchdir        133
#define __NR__llseek       140
#define __NR_msync         144
#define __NR_mlock         150
#define __NR_munlock       151
#define __NR_mlockall      152
#define __NR_munlockall    153
#define __NR_rt_sigreturn  173
#define __NR_rt_sigaction  174
#define __NR_rt_sigprocmask 175
#define __NR_rt_sigpending 176
#define __NR_rt_sigsuspend 179
#define __NR_sigaltstack   186
#define __NR_vfork         190
#define __NR_ugetrlimit    191
#define __NR_getuid32      199
#define __NR_getgid32      200
#define __NR_geteuid32     201
#define __NR_getegid32     202
#define __NR_getgroups32   205
#define __NR_setgroups32   206
#define __NR_setuid32      213
#define __NR_setgid32      214
#define __NR_getdents64    220
#define __NR_madvise       238
#define __NR_clock_gettime 260
#define __NR_statfs64      263
#define __NR_fstatfs64     264
#define __NR_openat        288
#define __NR_mkdirat       289
#define __NR_unlinkat      294
#define __NR_renameat      295
#define __NR_symlinkat     297
#define __NR_readlinkat    298
#define __NR_fchmodat      299
#define __NR_faccessat     300
#define __NR_dup3          326
#define __NR_pipe2         327
#define __NR_prlimit64     339
#define __NR_getrandom     352
#define __NR_statx         379

/* The *at calls' directory argument, and their flags. */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200   /* unlinkat: act as rmdir          */
#define AT_EACCESS          0x200   /* faccessat                        */
#define AT_SYMLINK_FOLLOW   0x400
#define AT_NO_AUTOMOUNT     0x800
#define AT_EMPTY_PATH       0x1000  /* statx on the descriptor itself   */
#define AT_STATX_SYNC_TYPE  0x6000

/* statx: Linux's struct, 256 bytes. */
struct statx_timestamp {
    s64 tv_sec;
    u32 tv_nsec;
    s32 __reserved;
};

struct statx {
    u32 stx_mask;
    u32 stx_blksize;
    u64 stx_attributes;
    u32 stx_nlink;
    u32 stx_uid;
    u32 stx_gid;
    u16 stx_mode;
    u16 __spare0;
    u64 stx_ino;
    u64 stx_size;
    u64 stx_blocks;
    u64 stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    u32 stx_rdev_major;
    u32 stx_rdev_minor;
    u32 stx_dev_major;
    u32 stx_dev_minor;
    u64 __spare2[14];
};

#define STATX_TYPE          0x0001
#define STATX_MODE          0x0002
#define STATX_NLINK         0x0004
#define STATX_UID           0x0008
#define STATX_GID           0x0010
#define STATX_ATIME         0x0020
#define STATX_MTIME         0x0040
#define STATX_CTIME         0x0080
#define STATX_INO           0x0100
#define STATX_SIZE          0x0200
#define STATX_BLOCKS        0x0400
#define STATX_BASIC_STATS   0x07ff

/* getdents64: variable-length records, each 8-byte aligned. */
struct linux_dirent64 {
    u64  d_ino;
    s64  d_off;
    u16  d_reclen;
    u8   d_type;
    char d_name[1];             /* NUL-terminated, then padding */
} __attribute__((packed));

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_SOCK     12

/* clock_gettime */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

/* getrlimit and friends. RLIM_INFINITY is all ones in either width. */
struct rlimit {
    u32 rlim_cur;
    u32 rlim_max;
};

struct rlimit64 {
    u64 rlim_cur;
    u64 rlim_max;
};

#define RLIM_INFINITY   0xffffffffUL
#define RLIMIT_CPU      0
#define RLIMIT_FSIZE    1
#define RLIMIT_DATA     2
#define RLIMIT_STACK    3
#define RLIMIT_CORE     4
#define RLIMIT_RSS      5
#define RLIMIT_NPROC    6
#define RLIMIT_NOFILE   7
#define RLIMIT_MEMLOCK  8
#define RLIMIT_AS       9
#define RLIM_NLIMITS    16

/* wait4's resource usage, 72 bytes. Only the times are filled in. */
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    s32 ru_other[14];
};

/*
 * The rt_ signal calls. Linux/m68k's struct sigaction for these puts
 * the flags second and the mask last, and the mask is 64 bits; signals
 * above 31 do not exist here, so the upper word is ignored going in and
 * zero coming out. sigsetsize must be 8, as on Linux.
 *
 * Linux/m68k does not define SA_RESTORER, and ignores sa_restorer:
 * the kernel writes a two-instruction trampoline onto the stack and
 * returns through that. This kernel does the same unless SA_RESTORER is
 * set, which lib/ulib does, pointing at a trampoline in crt0.s.
 */
struct kernel_sigaction {
    sighandler_t sa_handler;
    u32          sa_flags;
    void       (*sa_restorer)(void);
    u32          sa_mask[2];
};

#define SA_NOCLDWAIT    0x00000002
#define SIGSET_BYTES    8

typedef struct {
    void *ss_sp;
    int   ss_flags;
    u32   ss_size;
} stack_t;

/* What an SA_SIGINFO handler gets as its second argument: 128 bytes. */
struct siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        int _pad[29];
        struct {
            int si_pid;
            u32 si_uid;
        } _kill;
        struct {
            int si_pid;
            u32 si_uid;
            int si_status;
            s32 si_utime;
            s32 si_stime;
        } _chld;
    } _sifields;
};

#define SI_USER     0
#define SI_KERNEL   0x80

/*
 * And as its third: Linux/m68k's ucontext. The registers are d0-d7,
 * a0-a6, the user stack pointer, pc and sr, in that order. The FPU's
 * visible registers are in fpregs; its state frame (fsave) goes in the
 * first words of uc_filler, where Linux/m68k keeps it too. A handler
 * that changes any of them changes what the interrupted code resumes
 * with.
 */
struct m68k_mcontext {
    int version;                /* MCONTEXT_VERSION */
    int gregs[18];
    struct {
        int f_fpcntl[3];        /* fpcr, fpsr, fpiar */
        int f_fpregs[8 * 3];    /* fp0-fp7, 96-bit extended each */
    } fpregs;
};

#define MCONTEXT_VERSION 2

struct ucontext {
    u32                  uc_flags;
    struct ucontext     *uc_link;
    stack_t              uc_stack;
    struct m68k_mcontext uc_mcontext;
    u32                  uc_filler[80];
    u32                  uc_sigmask[2];
};

/* getrandom flags: accepted; the pool never blocks. */
#define GRND_NONBLOCK   0x0001
#define GRND_RANDOM     0x0002

/* reboot() commands, Linux's magic values cut down to what is useful. */
#define RB_HALT_SYSTEM  0xcdef0123
#define RB_AUTOBOOT     0x01234567
#define RB_POWER_OFF    0x4321fedc

#endif /* UAPI_H */
