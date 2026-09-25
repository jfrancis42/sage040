/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tty.c - the line discipline, once per terminal.
 *
 * This used to be ONE console that fanned every byte out to the screen
 * AND the serial line and merged their input, so the machine had a
 * single terminal wearing two faces. That is what made the size wrong:
 * a full-screen program had to fit on the smaller of the two, so a
 * 640x480 screen that is really 80x30 was clamped to the serial line's
 * assumed 80x24 and the bottom of the screen was wasted.
 *
 * There are two INDEPENDENT terminals now, each with its own line
 * discipline -- its own input ring, termios, foreground group and size:
 *
 *   tty1      the screen: keyboard in, framebuffer out. 80x30, because
 *             that is what the framebuffer measures, and nothing else
 *             gets a vote.
 *   console   the serial line: /dev/ttyS0's UART in and out. 80x24
 *             until `stty`/`resize` says otherwise. Kernel messages go
 *             here.
 *
 * A getty runs on each, so you log in wherever you are sitting; neither
 * echoes onto the other. `struct tty` is the whole of a terminal, and
 * the machine has two of them.
 *
 * What a terminal does (unchanged from when there was one):
 *
 *   read()   canonical mode assembles a line, echoing, with erase and
 *            kill; raw mode hands over whatever has arrived. Input comes
 *            from any of this terminal's sources.
 *   write()  turns \n into \r\n and sends it to this terminal's output.
 *   signals  INTR and SUSP go to this terminal's foreground group.
 *
 * Interrupt-driven input: a source whose driver takes its receive
 * interrupt is marked, and the interrupt calls tty_input_irq(dev),
 * which drains that source's terminal. A source with no interrupt (the
 * screen's own VT replies) is polled, from the reader and the tick.
 */
#include "poll.h"
#include "tty.h"
#include "dev.h"
#include "vfs.h"
#include "task.h"
#include "signal.h"
#include "wait.h"
#include "console.h"
#include "errno.h"
#include "string.h"

#define CTRL(x)    ((x) & 0x1f)
#define IN_RING    256
#define TTY_SRC_MAX 3

struct tty {
    const char *name;                   /* the /dev node                */
    struct chardev dev;                 /* this terminal as a device    */

    struct chardev *src[TTY_SRC_MAX];   /* where input comes from       */
    u8    src_irq[TTY_SRC_MAX];         /* which of them interrupt      */
    int   nsrc;
    struct chardev *out;                /* where output goes (one)      */

    struct termios tio;

    u8    ring[IN_RING];                /* interrupt-driven input        */
    u32   head, tail, overruns;
    int   stalled;
    int   pushback;                     /* one char taken and not wanted */
    struct waitq wait;

    int   fg_pgrp;                      /* ctrl-C's target here          */
    int   pending_sig;                  /* a ctrl-C waiting for a group  */

    struct winsize ws;                  /* assumed size; a self-sizing
                                         * output overrides it           */
    int   selfsize;                     /* the output measures itself    */
};

/*
 * THE TWO TERMINALS.
 *
 * Their termios starts canonical, echoing, signals on -- what a shell
 * hands a program, and what each looks like at boot. The serial line is
 * a VT100's 80x24 until told otherwise; the screen's size is taken from
 * the framebuffer when it is wired, so the {0,0} here is a placeholder
 * the wiring fills.
 */
#define TIO_DEFAULT { \
    ICRNL, OPOST | ONLCR, 0, ISIG | ICANON | ECHO, 0, \
    { CTRL('C'), CTRL('\\'), 0x7f, CTRL('U'), CTRL('D'), 0, 1, 0, 0, 0, \
      CTRL('Z'), 0, 0, 0, 0, 0, 0, 0, 0 } }

static struct tty tty_screen = {
    .name = "tty1", .pushback = -1, .tio = TIO_DEFAULT,
    .ws = { 30, 80, 0, 0 }, .selfsize = 1,
};
static struct tty tty_serial = {
    .name = "console", .pushback = -1, .tio = TIO_DEFAULT,
    .ws = { 24, 80, 0, 0 }, .selfsize = 0,
};
static struct tty *ttys[] = { &tty_screen, &tty_serial };
#define NTTY ((int)(sizeof(ttys) / sizeof(ttys[0])))

/* Which terminal a raw device belongs to, by name: the keyboard and the
 * framebuffer are the screen's; the serial UART is the serial line's. */
static struct tty *tty_for_dev(const struct chardev *d)
{
    if (!d) {
        return 0;
    }
    if (strcmp(d->name, "ttyS0") == 0) {
        return &tty_serial;
    }
    return &tty_screen;         /* kbd0, fbcon */
}

/* The terminal a descriptor is open on: its priv is the struct tty. */
static struct tty *tty_of(struct file *f)
{
    return f ? (struct tty *)f->priv : 0;
}

static int ring_empty(struct tty *t)
{
    return t->head == t->tail;
}

/* ---------------------------------------------------------------- */
/* Foreground group and read permission                              */
/* ---------------------------------------------------------------- */

static void set_foreground(struct tty *t, int pgrp)
{
    int sig = t->pending_sig;

    t->fg_pgrp = pgrp;
    t->pending_sig = 0;
    if (sig && pgrp && (!current || pgrp != current->pgid)) {
        signal_group(pgrp, sig);
    }
}

/*
 * tty_set_foreground kept for the boot task, which hands its terminal
 * to the first getty it starts. It acts on the SERIAL terminal, because
 * that is where the boot task's own descriptors are (kernel messages).
 * Per-terminal handoff after that goes through TIOCSPGRP on the
 * terminal's own descriptor.
 */
void tty_set_foreground(int pgrp)
{
    set_foreground(&tty_serial, pgrp);
}

/*
 * May the current task read this terminal? 0 if so. A task outside the
 * foreground group is sent SIGTTIN (default: stop); the read returns
 * -EINTR and restarts on `fg`. One that ignores or blocks SIGTTIN gets
 * EIO instead, POSIX's rule, so it does not spin on the signal.
 */
static int may_read(struct tty *t)
{
    struct task *c = current;

    if (!c || !c->as || c->pgid == t->fg_pgrp) {
        return 0;
    }
    if ((c->sig_blocked & SIGMASK(SIGTTIN)) ||
        c->sigact[SIGTTIN].sa_handler == SIG_IGN) {
        return -EIO;
    }
    signal_group(c->pgid, SIGTTIN);
    return -EINTR;
}

/* ---------------------------------------------------------------- */
/* Talking to the devices underneath                                 */
/* ---------------------------------------------------------------- */

static void as_file(struct file *f, struct chardev *d)
{
    memset(f, 0, sizeof(*f));
    f->ops = d->ops;
    f->priv = d->priv;
    f->used = 1;
}

static void raw_write(struct chardev *d, const void *buf, u32 len)
{
    struct file f;

    if (!d || !d->ops->write) {
        return;
    }
    as_file(&f, d);
    d->ops->write(&f, buf, len);
}

static int source_ready(struct chardev *d)
{
    struct file f;
    u32 n = 0;

    if (!d->ops->ioctl) {
        return 0;
    }
    as_file(&f, d);
    if (d->ops->ioctl(&f, FIONREAD, (u32)&n) < 0) {
        return 0;
    }
    return n > 0;
}

static int source_get(struct chardev *d)
{
    struct file f;
    u8 c;

    if (!d->ops->read) {
        return -1;
    }
    as_file(&f, d);
    if (d->ops->read(&f, &c, 1) != 1) {
        return -1;
    }
    return (int)c;
}

/* ---------------------------------------------------------------- */
/* Output                                                            */
/* ---------------------------------------------------------------- */

/*
 * To this terminal's one output, ONLCR applied here rather than in the
 * device: a device writes bytes; deciding a newline needs a carriage
 * return in front of it is the terminal's business.
 */
static void out_write(struct tty *t, const void *buf, u32 len)
{
    const u8 *p = buf;
    u32 start = 0, i;

    if (!t->out) {
        return;
    }
    if (!(t->tio.c_oflag & OPOST) || !(t->tio.c_oflag & ONLCR)) {
        raw_write(t->out, p, len);
        return;
    }
    for (i = 0; i <= len; i++) {
        if (i == len || p[i] == '\n') {
            if (i > start) {
                raw_write(t->out, p + start, i - start);
            }
            if (i < len) {
                raw_write(t->out, "\r\n", 2);
            }
            start = i + 1;
        }
    }
}

static s32 tty_write(struct file *f, const void *buf, u32 len)
{
    struct tty *t = tty_of(f);

    if (t) {
        out_write(t, buf, len);
    }
    return (s32)len;
}

static void echo(struct tty *t, int c)
{
    u8 ch = (u8)c;

    if (t->tio.c_lflag & ECHO) {
        out_write(t, &ch, 1);
    }
}

/* ---------------------------------------------------------------- */
/* Input                                                             */
/* ---------------------------------------------------------------- */

static int any_ready(struct tty *t)
{
    int i;

    if (t->pushback >= 0 || !ring_empty(t)) {
        return 1;
    }
    for (i = 0; i < t->nsrc; i++) {
        if (!t->src_irq[i] && source_ready(t->src[i])) {
            return 1;
        }
    }
    return 0;
}

static void drain_irq(struct tty *t);   /* forward */

/*
 * One character if there is one, or -1. Never waits. MASKED against the
 * timer: the tick's tty_poll_signals() may take a character and put it
 * in the pushback slot; between a reader checking that slot and taking
 * one from the device, the two could come out reversed -- SHELL as
 * SHLEL, once in a few thousand -- so the whole of this is atomic.
 */
static int poll_char(struct tty *t)
{
    u16 sr = irq_save();
    int c = -1, i;

    if (t->pushback >= 0) {
        c = t->pushback;
        t->pushback = -1;
        irq_restore(sr);
        return c;
    }
    if (!ring_empty(t)) {
        c = t->ring[t->tail % IN_RING];
        t->tail++;
        /* Room again: take what the chip held back. It will not
         * interrupt again until it has been emptied. */
        if (t->stalled && t->head - t->tail < IN_RING / 2) {
            t->stalled = 0;
            drain_irq(t);
        }
        irq_restore(sr);
        return c;
    }
    for (i = 0; i < t->nsrc; i++) {
        if (!t->src_irq[i] && source_ready(t->src[i])) {
            c = source_get(t->src[i]);
            if (c >= 0) {
                irq_restore(sr);
                return c;
            }
        }
    }
    irq_restore(sr);
    return -1;
}

/*
 * One character, waiting for it. Sleeps rather than spins; the timer
 * wakes it through tty_poll_signals(). -EINTR if a signal arrived
 * instead, which is what makes ctrl-C reach a blocked read.
 */
static int next_char(struct tty *t)
{
    for (;;) {
        int c = poll_char(t);

        if (c >= 0) {
            return c;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        sleep_on_timeout(&t->wait, 100);
    }
}

/* The signal this character means, if it means one; 0 if not. Checked
 * in both read paths, because a program in raw mode (readline clears
 * ICANON and ECHO but leaves ISIG) still expects ctrl-C. */
static int signal_of(struct tty *t, int c)
{
    if (!(t->tio.c_lflag & ISIG)) {
        return 0;
    }
    if (t->tio.c_cc[VINTR] && c == t->tio.c_cc[VINTR]) {
        return SIGINT;
    }
    if (t->tio.c_cc[VSUSP] && c == t->tio.c_cc[VSUSP]) {
        return SIGTSTP;
    }
    return 0;
}

/* To this terminal's foreground group. Called from a reader, which has
 * dealt with the key, so nothing is left pending. */
static int signal_char(struct tty *t, int c)
{
    int sig = signal_of(t, c);

    if (!sig) {
        return 0;
    }
    signal_group(t->fg_pgrp, sig);
    t->pending_sig = 0;
    return sig;
}

static s32 tty_read_raw(struct tty *t, u8 *out, u32 len)
{
    u32 n = 0;

    for (;;) {
        int c = (n == 0) ? next_char(t) : poll_char(t);

        if (c < 0) {
            break;
        }
        if (c == -EINTR) {
            return n > 0 ? (s32)n : -EINTR;
        }
        if (signal_char(t, c)) {
            return n > 0 ? (s32)n : -EINTR;
        }
        if (c == '\r' && (t->tio.c_iflag & ICRNL)) {
            c = '\n';
        }
        echo(t, c);
        out[n++] = (u8)c;
        if (n == len) {
            break;
        }
    }
    return (s32)n;
}

static s32 tty_read_canon(struct tty *t, u8 *out, u32 len)
{
    u32 n = 0;

    for (;;) {
        int c = next_char(t);

        if (c == -EINTR || signal_char(t, c)) {
            return -EINTR;
        }
        if (c == '\r' && (t->tio.c_iflag & ICRNL)) {
            c = '\n';
        }
        if (t->tio.c_cc[VEOF] && c == t->tio.c_cc[VEOF]) {
            return (s32)n;              /* end of input on an empty line */
        }
        if ((t->tio.c_cc[VERASE] && c == t->tio.c_cc[VERASE]) || c == '\b') {
            if (n > 0) {
                n--;
                if (t->tio.c_lflag & ECHO) {
                    out_write(t, "\b \b", 3);
                }
            }
            continue;
        }
        if (t->tio.c_cc[VKILL] && c == t->tio.c_cc[VKILL]) {
            while (n > 0) {
                n--;
                if (t->tio.c_lflag & ECHO) {
                    out_write(t, "\b \b", 3);
                }
            }
            continue;
        }
        if (c == '\n') {
            out[n++] = '\n';
            echo(t, '\n');
            return (s32)n;
        }
        if (c < 32 || c == 127) {
            continue;                   /* controls are not input; >127
                                         * is UTF-8 */
        }
        if (n < len) {
            out[n++] = (u8)c;
            echo(t, c);
            if (n == len) {
                return (s32)n;
            }
        }
    }
}

static s32 tty_read(struct file *f, void *buf, u32 len)
{
    struct tty *t = tty_of(f);
    int err;

    if (!t || len == 0) {
        return 0;
    }
    err = may_read(t);
    if (err < 0) {
        return err;
    }
    if (t->tio.c_lflag & ICANON) {
        return tty_read_canon(t, buf, len);
    }
    return tty_read_raw(t, buf, len);
}

/* ---------------------------------------------------------------- */
/* Interrupt-driven input and the tick                               */
/* ---------------------------------------------------------------- */

/* Drain every interrupt-driven source of ONE terminal into its ring,
 * acting on ctrl-C/ctrl-Z as it goes. The chip's interrupt is an edge:
 * a character left behind holds the line high and never makes another,
 * so this takes everything. */
static void drain_irq(struct tty *t)
{
    int i, got = 0;

    for (i = 0; i < t->nsrc; i++) {
        struct chardev *d = t->src[i];
        int n;

        if (!t->src_irq[i]) {
            continue;
        }
        for (n = 0; n < 4096 && source_ready(d); n++) {
            int c, sig;

            if (t->head - t->tail >= IN_RING) {
                t->stalled = 1;         /* full: leave the rest in the chip */
                break;
            }
            c = source_get(d);
            if (c < 0) {
                break;
            }
            /* ctrl-C/ctrl-Z act NOW if a program is in the foreground;
             * otherwise the character goes in the ring for the reader
             * (the shell's editor abandons its line) and is kept as
             * pending_sig for a job the shell is about to start. */
            sig = signal_of(t, c);
            if (sig) {
                if (signal_group(t->fg_pgrp, sig) == 0) {
                    continue;
                }
                t->pending_sig = sig;
            }
            t->ring[t->head % IN_RING] = (u8)c;
            t->head++;
            got = 1;
        }
    }
    if (got) {
        wake_all(&t->wait);
        poll_wake();
    }
}

/* Called from a source's receive interrupt: the driver passes its own
 * device, and this drains that device's terminal. */
void tty_input_irq(struct chardev *d)
{
    struct tty *t = tty_for_dev(d);

    if (t) {
        drain_irq(t);
    }
}

/* Called from the timer. For each terminal, look for a ctrl-C on its
 * non-interrupt sources, and wake anything waiting for input. */
void tty_poll_signals(void)
{
    int k;

    for (k = 0; k < NTTY; k++) {
        struct tty *t = ttys[k];
        int c, i;

        if (t->pushback < 0) {
            u16 sr = irq_save();

            c = -1;
            for (i = 0; i < t->nsrc && c < 0; i++) {
                if (!t->src_irq[i] && source_ready(t->src[i])) {
                    c = source_get(t->src[i]);
                }
            }
            irq_restore(sr);
            if (c >= 0 && !signal_char(t, c)) {
                t->pushback = c;
            }
        }
        if (t->pushback >= 0 || any_ready(t)) {
            wake_all(&t->wait);
            poll_wake();
        }
    }
}

/* ---------------------------------------------------------------- */
/* Size                                                              */
/* ---------------------------------------------------------------- */

/*
 * This terminal's size. A self-sizing output (the framebuffer) is asked
 * every time, so it is always the screen's real geometry; otherwise the
 * assumed size, which TIOCSWINSZ (stty/resize) sets. No min of two
 * outputs any more -- each terminal has exactly one.
 */
static void get_ws(struct tty *t, struct winsize *out)
{
    *out = t->ws;
    if (t->selfsize && t->out && t->out->ops->ioctl) {
        struct file f;
        struct winsize w;

        as_file(&f, t->out);
        memset(&w, 0, sizeof(w));
        if (t->out->ops->ioctl(&f, TIOCGWINSZ, (u32)&w) == 0 &&
            w.ws_row && w.ws_col) {
            *out = w;
        }
    }
}

/* ---------------------------------------------------------------- */
/* ioctl                                                             */
/* ---------------------------------------------------------------- */

static int tty_ioctl(struct file *f, u32 request, u32 arg)
{
    struct tty *t = tty_of(f);

    if (!t) {
        return -ENOTTY;
    }
    switch (request) {
    case FIONREAD:
        if (arg) {
            *(u32 *)arg = any_ready(t) ? 1 : 0;
        }
        return 0;

    case TIOCGPGRP:
        *(int *)arg = t->fg_pgrp;
        return 0;

    case TIOCSPGRP: {
        int pg = *(int *)arg;
        struct task *task;
        int i;

        for (i = 0; (task = task_nth(i)) != 0; i++) {
            if (task->pgid == pg && task->state != TASK_ZOMBIE) {
                /* set_foreground, not a bare assignment: taking the
                 * terminal hands over a ctrl-C that landed in
                 * pending_sig while no group had it (the shell is a
                 * kernel task and takes no signals). Without this a
                 * foreground pipeline that should die on ctrl-C hangs. */
                set_foreground(t, pg);
                return 0;
            }
        }
        return -EPERM;
    }

    case TIOCGWINSZ:
        if (!arg) {
            return -EINVAL;
        }
        get_ws(t, (struct winsize *)arg);
        return 0;

    case TIOCSWINSZ: {
        struct winsize before, after;

        if (!arg) {
            return -EINVAL;
        }
        get_ws(t, &before);
        t->ws = *(const struct winsize *)arg;
        get_ws(t, &after);
        if (after.ws_row != before.ws_row || after.ws_col != before.ws_col) {
            signal_group(t->fg_pgrp, SIGWINCH);
        }
        return 0;
    }

    case TCGETS:
        if (!arg) {
            return -EINVAL;
        }
        *(struct termios *)arg = t->tio;
        return 0;

    case TCGETS2: {
        struct termios2 *t2 = (struct termios2 *)arg;

        if (!t2) {
            return -EINVAL;
        }
        memcpy(t2, &t->tio, sizeof(t->tio));
        t2->c_ispeed = t2->c_ospeed = 38400;
        return 0;
    }

    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2: {
        struct termios tt;

        if (!arg) {
            return -EINVAL;
        }
        memcpy(&tt, (const void *)arg, sizeof(tt));
        return tty_ioctl(f, request == TCSETSF2 ? TCSETSF : TCSETS, (u32)&tt);
    }

    case TCSETSF:
        t->pushback = -1;
        while (any_ready(t)) {
            (void)poll_char(t);
        }
        /* fall through */
    case TCSETS:
    case TCSETSW:
        if (!arg) {
            return -EINVAL;
        }
        t->tio = *(const struct termios *)arg;
        return 0;

    default:
        return -ENOTTY;
    }
}

static int tty_close(struct file *f)
{
    (void)f;
    return 0;                   /* the terminal outlives every descriptor */
}

static int tty_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR;
    st->st_size = 0;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static const struct file_ops tty_ops = {
    tty_read, tty_write, 0, tty_ioctl, tty_close, tty_fstat, 0, 0, 0,
};

/* ---------------------------------------------------------------- */
/* Registration and wiring                                           */
/* ---------------------------------------------------------------- */

int tty_add_source(struct chardev *d)
{
    struct tty *t = tty_for_dev(d);

    if (!d || !d->ops || !d->ops->read) {
        return -EINVAL;
    }
    if (t->nsrc == TTY_SRC_MAX) {
        return -ENOSPC;
    }
    t->src_irq[t->nsrc] = 0;
    t->src[t->nsrc++] = d;
    return 0;
}

int tty_add_sink(struct chardev *d)
{
    struct tty *t = tty_for_dev(d);

    if (!d || !d->ops || !d->ops->write) {
        return -EINVAL;
    }
    t->out = d;                 /* one output; no fan-out, no mirroring */
    return 0;
}

int tty_source_irq(struct chardev *d)
{
    struct tty *t = tty_for_dev(d);
    int i;

    for (i = 0; i < t->nsrc; i++) {
        if (t->src[i] == d) {
            t->src_irq[i] = 1;
            /* Anything that arrived before the interrupt was on holds
             * the line high and makes no edge: take it now. */
            {
                u16 sr = irq_save();

                drain_irq(t);
                irq_restore(sr);
            }
            return 0;
        }
    }
    return -ENOENT;
}

u32 tty_overruns(void)
{
    return tty_screen.overruns + tty_serial.overruns;
}

/* Reporting, for the boot banner: each terminal, its output and its
 * sources. index runs over (terminal, kind) pairs. */
struct chardev *tty_nth(int index, const char **ttyname, int *is_source)
{
    int k, i, seen = 0;

    for (k = 0; k < NTTY; k++) {
        struct tty *t = ttys[k];

        if (t->out) {
            if (seen++ == index) {
                *ttyname = t->name; *is_source = 0; return t->out;
            }
        }
        for (i = 0; i < t->nsrc; i++) {
            if (seen++ == index) {
                *ttyname = t->name; *is_source = 1; return t->src[i];
            }
        }
    }
    return 0;
}

struct chardev *tty_device(void)
{
    return &tty_serial.dev;     /* /dev/console: the serial line */
}

/*
 * Put a terminal back the way a shell hands it to a program, after one
 * that set raw mode was killed before restoring it. Resets the terminal
 * of the given descriptor.
 */
void tty_reset_fd(int fd)
{
    struct file *f = fd_get(fd);
    struct tty *t = f ? tty_of(f) : 0;

    if (t && f->ops == &tty_ops) {
        t->tio.c_iflag = ICRNL;
        t->tio.c_oflag = OPOST | ONLCR;
        t->tio.c_lflag = ISIG | ICANON | ECHO;
    }
}

static void register_tty(struct tty *t)
{
    t->dev.name = t->name;
    t->dev.ops = &tty_ops;
    t->dev.priv = t;
    t->dev.next = 0;
    dev_register_char(&t->dev);
}

int tty_init(void)
{
    tty_screen.pushback = -1;
    tty_serial.pushback = -1;

    register_tty(&tty_screen);          /* /dev/tty1  */
    register_tty(&tty_serial);          /* /dev/console */

    /*
     * Kernel messages and the boot task's descriptors go to the SERIAL
     * line -- that is where a headless machine and every test watches,
     * and it is up before the screen. The screen's getty rebinds its
     * own descriptors when it starts.
     */
    console_set(&tty_serial.dev);
    fd_bind(0, &tty_ops, &tty_serial, O_RDONLY);
    fd_bind(1, &tty_ops, &tty_serial, O_WRONLY);
    fd_bind(2, &tty_ops, &tty_serial, O_WRONLY);
    return 0;
}

/* Bind the current task's standard descriptors to a named terminal --
 * how a getty attaches to the line it serves. Returns -ENODEV if there
 * is no such terminal. */
int tty_attach(const char *name)
{
    int k;

    for (k = 0; k < NTTY; k++) {
        if (strcmp(ttys[k]->name, name) == 0) {
            fd_bind(0, &tty_ops, ttys[k], O_RDONLY);
            fd_bind(1, &tty_ops, ttys[k], O_WRONLY);
            fd_bind(2, &tty_ops, ttys[k], O_WRONLY);
            /* Become this terminal's foreground group, so the getty and
             * the login it spawns may read it without SIGTTIN. */
            if (current) {
                set_foreground(ttys[k], current->pgid);
            }
            return 0;
        }
    }
    return -ENODEV;
}
