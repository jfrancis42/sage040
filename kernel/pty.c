/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * pty.c - pseudo-terminals: a terminal with a program at each end.
 *
 * A pty is a pair of devices and a line discipline between them. What a
 * program writes to the MASTER arrives at the SLAVE as though it had
 * been typed; what the program on the slave writes comes back out of the
 * master as though it had been displayed. The slave is a terminal in
 * every way that matters -- isatty() says so, it has termios settings, a
 * window size, a foreground process group, and ctrl-C on it sends
 * SIGINT -- and the master is where a person, a network connection or a
 * test harness sits.
 *
 * WHY A MACHINE WANTS THEM. Every program that drives another program's
 * terminal needs one: `script`, `expect`, a terminal emulator, Python's
 * pty module, and -- the reason this exists now -- an ssh server, which
 * is a program that has to give a shell somewhere to be interactive.
 * Without a pty the only terminal on this machine is the one soldered to
 * the board.
 *
 * THE NAMES ARE LINUX'S. /dev/ptmx allocates a pair and gives back the
 * master; the slave is /dev/pts/N, where N comes from TIOCGPTN. The
 * VFS turns "/dev/<rest>" into a device lookup of <rest>, so a device
 * registered as "pts/0" IS /dev/pts/0 with no directory needed, which
 * is a small piece of luck worth spending.
 *
 * WHAT IS NOT HERE. No packet mode (TIOCPKT), no TIOCSTI, and the
 * output side does ONLCR and nothing else -- no tab expansion, no
 * delays. A program that needs those is asking the terminal to do work
 * that belongs in the program.
 */
#include "pty.h"
#include "kernel.h"
#include "dev.h"
#include "vfs.h"
#include "uapi.h"
#include "errno.h"
#include "wait.h"
#include "poll.h"
#include "task.h"
#include "signal.h"
#include "uaccess.h"
#include "string.h"

#define PTY_MAX     8           /* pairs; each costs two rings         */
#define PTY_BUF     2048        /* bytes each way                      */

struct ptyring {
    u8  buf[PTY_BUF];
    u32 head, tail;
    struct waitq wait;
};

struct pty {
    int used;
    int index;
    int master_open;
    int slave_open;             /* how many descriptors hold the slave */

    struct ptyring to_slave;    /* master writes, slave reads          */
    struct ptyring to_master;   /* slave writes, master reads          */

    struct termios tio;
    struct winsize win;
    int pgrp;                   /* the foreground group on this pty    */

    char name[12];              /* "pts/N"                             */
    struct chardev slave_dev;
    struct file_ops slave_ops;  /* a copy per pty: priv is the pty     */
};

static struct pty ptys[PTY_MAX];

/* --- the rings -------------------------------------------------------- */

static u32 ring_used(const struct ptyring *r)
{
    return r->head - r->tail;
}

static int ring_full(const struct ptyring *r)
{
    return ring_used(r) >= PTY_BUF;
}

static void ring_put(struct ptyring *r, u8 c)
{
    if (ring_full(r)) {
        /*
         * Drop it. A terminal has a finite input queue and a real one
         * beeps; there is nowhere to push back TO here, because the
         * writer is a program rather than a wire, and blocking the
         * master because a slave is not reading would deadlock a
         * program that does both.
         */
        return;
    }
    r->buf[r->head % PTY_BUF] = c;
    r->head++;
}

static int ring_get(struct ptyring *r)
{
    int c;

    if (r->head == r->tail) {
        return -1;
    }
    c = r->buf[r->tail % PTY_BUF];
    r->tail++;
    return c;
}

/* --- the line discipline ---------------------------------------------- */

/*
 * Echo goes back to the MASTER, which is where the person is: a slave
 * that echoed to itself would be writing to the program, not to
 * whoever typed.
 */
static void pty_echo(struct pty *p, u8 c)
{
    if (!(p->tio.c_lflag & ECHO)) {
        return;
    }
    if (c == '\n' && (p->tio.c_oflag & ONLCR)) {
        ring_put(&p->to_master, '\r');
    }
    ring_put(&p->to_master, c);
    wake_all(&p->to_master.wait);
}

/*
 * A character arriving from the master, through the input side of the
 * discipline. Returns 1 if it was consumed as a signal.
 */
static int pty_input(struct pty *p, u8 c)
{
    if ((p->tio.c_iflag & ICRNL) && c == '\r') {
        c = '\n';
    } else if ((p->tio.c_iflag & INLCR) && c == '\n') {
        c = '\r';
    }

    if (p->tio.c_lflag & ISIG) {
        int sig = 0;

        if (p->tio.c_cc[VINTR] && c == p->tio.c_cc[VINTR]) {
            sig = SIGINT;
        } else if (p->tio.c_cc[VQUIT] && c == p->tio.c_cc[VQUIT]) {
            sig = SIGQUIT;
        } else if (p->tio.c_cc[VSUSP] && c == p->tio.c_cc[VSUSP]) {
            sig = SIGTSTP;
        }
        if (sig) {
            /*
             * To the foreground group of THIS pty, which is what makes
             * ctrl-C in an ssh session interrupt the program there and
             * nothing on the console.
             */
            if (p->tio.c_lflag & ECHO) {
                ring_put(&p->to_master, '^');
                ring_put(&p->to_master, (u8)(c + 'A' - 1));
                ring_put(&p->to_master, '\r');
                ring_put(&p->to_master, '\n');
                wake_all(&p->to_master.wait);
            }
            if (p->pgrp) {
                signal_group(p->pgrp, sig);
            }
            return 1;
        }
    }

    if (p->tio.c_lflag & ICANON) {
        if ((p->tio.c_cc[VERASE] && c == p->tio.c_cc[VERASE]) || c == '\b') {
            /*
             * Erase only what this line has: the discipline's line is
             * whatever is in the ring past the last newline, so walk
             * back over one character unless the last one was a
             * newline (nothing to erase).
             */
            if (p->to_slave.head != p->to_slave.tail) {
                u8 last = p->to_slave.buf[(p->to_slave.head - 1) % PTY_BUF];

                if (last != '\n') {
                    p->to_slave.head--;
                    if (p->tio.c_lflag & ECHO) {
                        ring_put(&p->to_master, '\b');
                        ring_put(&p->to_master, ' ');
                        ring_put(&p->to_master, '\b');
                        wake_all(&p->to_master.wait);
                    }
                }
            }
            return 0;
        }
        if (p->tio.c_cc[VKILL] && c == p->tio.c_cc[VKILL]) {
            while (p->to_slave.head != p->to_slave.tail &&
                   p->to_slave.buf[(p->to_slave.head - 1) % PTY_BUF] != '\n') {
                p->to_slave.head--;
                if (p->tio.c_lflag & ECHO) {
                    ring_put(&p->to_master, '\b');
                    ring_put(&p->to_master, ' ');
                    ring_put(&p->to_master, '\b');
                }
            }
            wake_all(&p->to_master.wait);
            return 0;
        }
    }

    ring_put(&p->to_slave, c);
    pty_echo(p, c);
    if (!(p->tio.c_lflag & ICANON) || c == '\n' ||
        (p->tio.c_cc[VEOF] && c == p->tio.c_cc[VEOF])) {
        wake_all(&p->to_slave.wait);
    }
    return 0;
}

/*
 * In canonical mode a read returns only a whole line, so the slave has
 * to know whether one is there. Everything up to and including a
 * newline counts, and so does an end-of-file character.
 */
static int line_ready(struct pty *p)
{
    u32 i;

    if (!(p->tio.c_lflag & ICANON)) {
        return ring_used(&p->to_slave) > 0;
    }
    for (i = p->to_slave.tail; i != p->to_slave.head; i++) {
        u8 c = p->to_slave.buf[i % PTY_BUF];

        if (c == '\n' || (p->tio.c_cc[VEOF] && c == p->tio.c_cc[VEOF])) {
            return 1;
        }
    }
    return 0;
}

/* --- the slave -------------------------------------------------------- */

static struct pty *pty_of(struct file *f)
{
    return (struct pty *)f->priv;
}

static s32 pty_slave_read(struct file *f, void *buf, u32 len)
{
    struct pty *p = pty_of(f);
    u8 *out = buf;
    u32 n = 0;

    if (len == 0) {
        return 0;
    }
    while (!line_ready(p)) {
        if (!p->master_open) {
            return 0;           /* the other end has gone: end of file */
        }
        if (f->flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        sleep_on(&p->to_slave.wait);
        if (signal_pending(current)) {
            return -EINTR;
        }
    }
    while (n < len) {
        int c = ring_get(&p->to_slave);

        if (c < 0) {
            break;
        }
        if (p->tio.c_lflag & ICANON) {
            if (p->tio.c_cc[VEOF] && (u8)c == p->tio.c_cc[VEOF]) {
                break;          /* end of file, and not part of the data */
            }
            out[n++] = (u8)c;
            if (c == '\n') {
                break;
            }
        } else {
            out[n++] = (u8)c;
        }
    }
    return (s32)n;
}

static s32 pty_slave_write(struct file *f, const void *buf, u32 len)
{
    struct pty *p = pty_of(f);
    const u8 *in = buf;
    u32 i;

    for (i = 0; i < len; i++) {
        if (in[i] == '\n' && (p->tio.c_oflag & ONLCR)) {
            ring_put(&p->to_master, '\r');
        }
        ring_put(&p->to_master, in[i]);
    }
    wake_all(&p->to_master.wait);
    return (s32)len;
}

/*
 * THE ARGUMENT IS A KERNEL POINTER, not a user one.
 *
 * syscall.c keeps a table of the ioctls whose argument is a structure,
 * copies it in and out around the call, and hands the device a pointer
 * into its own buffer -- so a device dereferences it directly, as
 * tty.c does. A device that called copy_from_user here would be
 * reading the kernel's buffer as though it were a user address, which
 * fails in the quiet way: uaccess would refuse it and the ioctl would
 * report EFAULT for a perfectly good argument.
 */
static int pty_ioctl_common(struct pty *p, u32 req, u32 arg, int is_master)
{
    if (!arg) {
        return -EINVAL;
    }
    switch (req) {
    case TCGETS:
        *(struct termios *)arg = p->tio;
        return 0;

    case TCGETS2: {
        struct termios2 *t2 = (struct termios2 *)arg;

        memcpy(t2, &p->tio, sizeof(p->tio));
        /* There is no wire and so no speed; 38400 is what a terminal
         * with nothing to say about it reports, and it is what tty.c
         * says for the console. */
        t2->c_ispeed = t2->c_ospeed = 38400;
        return 0;
    }

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2:
        memcpy(&p->tio, (const void *)arg, sizeof(p->tio));
        if (req == TCSETSF || req == TCSETSF2) {
            /* Throw away what has been typed ahead: a program changing
             * modes does not want the old mode's leftovers read under
             * the new rules. */
            p->to_slave.tail = p->to_slave.head;
        }
        return 0;

    case TIOCGWINSZ:
        *(struct winsize *)arg = p->win;
        return 0;

    case TIOCSWINSZ:
        p->win = *(const struct winsize *)arg;
        /*
         * A terminal whose size changed tells the program on it, which
         * is the whole mechanism behind a window that can be resized.
         */
        if (p->pgrp) {
            signal_group(p->pgrp, SIGWINCH);
        }
        return 0;

    case TIOCGPGRP:
        *(int *)arg = p->pgrp;
        return 0;

    case TIOCSPGRP:
        p->pgrp = *(const int *)arg;
        return 0;

    case TIOCGPTN:
        /* Which pts this is. ptsname(3) asks the MASTER. */
        if (!is_master) {
            return -ENOTTY;
        }
        *(int *)arg = p->index;
        return 0;

    case TIOCSPTLCK:
        /* Linux locks a new slave until unlockpt(); nothing here could
         * open it in between, so this is accepted and does nothing
         * rather than refused. */
        return is_master ? 0 : -ENOTTY;

    case FIONREAD:
        *(u32 *)arg = is_master ? ring_used(&p->to_master)
                                : ring_used(&p->to_slave);
        return 0;

    default:
        return -ENOTTY;
    }
}

static int pty_slave_ioctl(struct file *f, u32 req, u32 arg)
{
    return pty_ioctl_common(pty_of(f), req, arg, 0);
}

static int pty_slave_poll(struct file *f)
{
    struct pty *p = pty_of(f);
    int ready = POLLOUT;

    if (line_ready(p)) {
        ready |= POLLIN;
    }
    if (!p->master_open) {
        ready |= POLLHUP;
    }
    return ready;
}

static int pty_slave_close(struct file *f)
{
    struct pty *p = pty_of(f);

    if (p->slave_open > 0) {
        p->slave_open--;
    }
    if (p->slave_open == 0) {
        /* The program on the terminal has gone: whoever is reading the
         * master sees end of file. */
        wake_all(&p->to_master.wait);
    }
    return 0;
}

static int pty_slave_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR | 0620;
    return 0;
}

/* --- the master ------------------------------------------------------- */

static s32 pty_master_read(struct file *f, void *buf, u32 len)
{
    struct pty *p = pty_of(f);
    u8 *out = buf;
    u32 n = 0;

    if (len == 0) {
        return 0;
    }
    while (ring_used(&p->to_master) == 0) {
        if (p->slave_open == 0) {
            return 0;           /* nothing on the terminal any more */
        }
        if (f->flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        sleep_on(&p->to_master.wait);
        if (signal_pending(current)) {
            return -EINTR;
        }
    }
    while (n < len) {
        int c = ring_get(&p->to_master);

        if (c < 0) {
            break;
        }
        out[n++] = (u8)c;
    }
    return (s32)n;
}

static s32 pty_master_write(struct file *f, const void *buf, u32 len)
{
    struct pty *p = pty_of(f);
    const u8 *in = buf;
    u32 i;

    for (i = 0; i < len; i++) {
        pty_input(p, in[i]);
    }
    /* Raw mode wakes on every character; canonical mode woke on the
     * newline inside pty_input. Waking again is harmless -- a reader
     * looks again and sleeps if there is still no line. */
    wake_all(&p->to_slave.wait);
    return (s32)len;
}

static int pty_master_ioctl(struct file *f, u32 req, u32 arg)
{
    return pty_ioctl_common(pty_of(f), req, arg, 1);
}

static int pty_master_poll(struct file *f)
{
    struct pty *p = pty_of(f);
    int ready = POLLOUT;

    if (ring_used(&p->to_master) > 0) {
        ready |= POLLIN;
    }
    if (p->slave_open == 0) {
        ready |= POLLHUP;
    }
    return ready;
}

/*
 * The last master closed. The slave's readers get end of file, and the
 * program on it is hung up on -- SIGHUP is what tells a shell its
 * terminal has gone, and without it an ssh session that lost its
 * connection would leave a shell running for ever.
 */
static int pty_master_close(struct file *f)
{
    struct pty *p = pty_of(f);

    if (p->master_open > 0) {
        p->master_open--;
    }
    if (p->master_open == 0) {
        if (p->pgrp) {
            signal_group(p->pgrp, SIGHUP);
        }
        wake_all(&p->to_slave.wait);
        if (p->slave_open == 0) {
            p->used = 0;
            dev_unregister_char(&p->slave_dev);
        }
    }
    return 0;
}

static const struct file_ops pty_master_ops = {
    pty_master_read,
    pty_master_write,
    0,                          /* lseek */
    pty_master_ioctl,
    pty_master_close,
    0,                          /* fstat */
    pty_master_poll,
    0,                          /* truncate */
    0,                          /* mmap */
};

/* --- /dev/ptmx -------------------------------------------------------- */

/*
 * Opening /dev/ptmx does not open a device: it ALLOCATES A PAIR and
 * hands back a descriptor onto the master of it. So this device's open
 * is the only interesting operation it has, and vfs.c calls it through
 * dev_open_hook.
 */
static void pty_defaults(struct pty *p)
{
    memset(&p->tio, 0, sizeof(p->tio));
    p->tio.c_iflag = ICRNL;
    p->tio.c_oflag = ONLCR;
    p->tio.c_cflag = CS8 | CREAD;
    p->tio.c_lflag = ICANON | ECHO | ISIG;
    p->tio.c_cc[VINTR] = 3;     /* ctrl-C */
    p->tio.c_cc[VQUIT] = 28;    /* ctrl-\ */
    p->tio.c_cc[VERASE] = 127;  /* DEL, as a terminal sends */
    p->tio.c_cc[VKILL] = 21;    /* ctrl-U */
    p->tio.c_cc[VEOF] = 4;      /* ctrl-D */
    p->tio.c_cc[VSUSP] = 26;    /* ctrl-Z */
    p->win.ws_row = 24;
    p->win.ws_col = 80;
    p->pgrp = 0;
}

int pty_open_master(struct file *f)
{
    int i;

    for (i = 0; i < PTY_MAX; i++) {
        struct pty *p = &ptys[i];

        if (p->used) {
            continue;
        }
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->index = i;
        pty_defaults(p);

        /* The slave, by the name Linux gives it. */
        p->slave_ops.read = pty_slave_read;
        p->slave_ops.write = pty_slave_write;
        p->slave_ops.ioctl = pty_slave_ioctl;
        p->slave_ops.close = pty_slave_close;
        p->slave_ops.fstat = pty_slave_fstat;
        p->slave_ops.poll = pty_slave_poll;
        strcpy(p->name, "pts/");
        p->name[4] = (char)('0' + i);
        p->name[5] = '\0';
        p->slave_dev.name = p->name;
        p->slave_dev.ops = &p->slave_ops;
        p->slave_dev.priv = p;
        if (dev_register_char(&p->slave_dev) < 0) {
            p->used = 0;
            return -ENFILE;
        }

        p->master_open = 1;
        f->ops = &pty_master_ops;
        f->priv = p;
        return 0;
    }
    return -ENFILE;             /* every pair is in use */
}

/* A slave was opened: count it, so the master can see it go. */
void pty_slave_opened(void *priv)
{
    struct pty *p = priv;

    if (p && p->used) {
        p->slave_open++;
    }
}

int pty_init(void)
{
    /*
     * /dev/ptmx itself. Its ops are never used for reading or writing:
     * vfs.c sees the open hook and replaces them with the master's.
     */
    static const struct file_ops ptmx_ops = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    static struct chardev ptmx = { "ptmx", &ptmx_ops, 0, 0 };

    return dev_register_char(&ptmx);
}
