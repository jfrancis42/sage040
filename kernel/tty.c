/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tty.c - the line discipline, the fan-out, and where ctrl-C goes.
 *
 * This used to live inside the NS16550A driver, which worked for exactly
 * as long as there was one place characters could come from and one
 * place they could go. There is now a screen and a keyboard as well as a
 * serial port, so it moved out to where it belongs.
 *
 * What a terminal does here:
 *
 *   read()   in canonical mode, assembles one line, echoing as it goes,
 *            with erase and kill doing what a person expects. In raw
 *            mode, hands over whatever has arrived as soon as anything
 *            has. Characters come from whichever source has one.
 *
 *   write()  turns a newline into carriage return and newline, and sends
 *            the result to every enabled sink.
 *
 *   signals  INTR and SUSP are recognised here and nowhere else, and go
 *            to the foreground job. This is the only part of the system
 *            that turns a keystroke into something that happens to a
 *            running program.
 *
 * The flags are termios flags, with Linux's names and Linux's numbers,
 * because they are the right vocabulary and because the line editor in
 * the shell drives them exactly as readline drives a real terminal:
 * ICANON and ECHO off, edit, put them back. None of the editing is in
 * here. That is deliberate -- bash's line editor is in userspace, and
 * this one moves there unchanged the day programs stop running as the
 * kernel.
 *
 * A device is a source if its ioctl answers FIONREAD, which is how this
 * asks "is there a character waiting" without committing to a read that
 * would block. Polling rather than interrupts, for now: when the UART
 * and the keyboard are both interrupt-driven they should feed one ring
 * buffer and this loop becomes a drain of it, which is a change inside
 * this file and nowhere else.
 */
#include "tty.h"
#include "dev.h"
#include "vfs.h"
#include "job.h"
#include "console.h"
#include "errno.h"
#include "string.h"

#define CTRL(x)    ((x) & 0x1f)

struct sink {
    struct chardev *dev;
    int enabled;
};

static struct chardev *sources[TTY_MAX_SOURCES];
static int nsources;

static struct sink sinks[TTY_MAX_SINKS];
static int nsinks;

static struct chardev tty_dev;
static struct chardev tty_alias;

/*
 * The current settings.
 *
 * Canonical, echoing, signals on, both newline translations on: what a
 * terminal looks like when a shell hands it to a program, and what this
 * one looks like at boot.
 */
static struct termios tio = {
    ICRNL,                      /* c_iflag */
    OPOST | ONLCR,              /* c_oflag */
    0,                          /* c_cflag: no baud rate to set here */
    ISIG | ICANON | ECHO,       /* c_lflag */
    0,                          /* c_line  */
    {
        CTRL('C'),              /* VINTR  */
        CTRL('\\'),             /* VQUIT  */
        0x7f,                   /* VERASE: DEL, as a modern terminal sends */
        CTRL('U'),              /* VKILL  */
        CTRL('D'),              /* VEOF   */
        0, 1, 0, 0, 0,          /* VTIME, VMIN, ...                       */
        CTRL('Z'),              /* VSUSP  */
        0, 0, 0, 0, 0, 0, 0, 0
    }
};

/*
 * One character that was taken from a source and not wanted yet.
 *
 * The timer tick looks for a pending ctrl-C while a program is running,
 * and the only way to tell whether a waiting character is one is to take
 * it. Anything else it finds goes here and is handed to the next reader,
 * in order, as though it had never been touched.
 */
static int pushback = -1;

/* What to do while nothing is being typed. See tty.h. */
static void (*idle_fn)(void);

void tty_set_idle(void (*fn)(void))
{
    idle_fn = fn;
}

/*
 * Set while a reader is inside next_char().
 *
 * The tick must not go poking at the same chip a foreground read is
 * already draining -- two readers of one FIFO lose characters between
 * them. While this is set the tick does not scan at all, which costs
 * nothing: if somebody is reading, the read will see the ctrl-C itself.
 */
static volatile int reader_active;

/* ---------------------------------------------------------------- */
/* Talking to the devices underneath                                 */
/* ---------------------------------------------------------------- */

/*
 * The devices are plain chardevs, so calling one means having a
 * `struct file` to call it with. Building a throwaway one is cheaper
 * than giving every driver a second entry point that does not need a
 * descriptor, and it keeps the layer below this identical to the layer
 * a program sees.
 */
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

    if (!d->ops->write) {
        return;
    }
    as_file(&f, d);
    d->ops->write(&f, buf, len);
}

/* Is a character waiting on this source? */
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

/* Take one, having already established there is one. */
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
 * To every enabled sink, with ONLCR applied once here rather than in
 * each of them. A sink writes bytes; deciding that a newline needs a
 * carriage return in front of it is the terminal's business.
 */
static void sink_write(const void *buf, u32 len)
{
    const u8 *p = buf;
    u32 start = 0, i;
    int s;

    if (!(tio.c_oflag & OPOST) || !(tio.c_oflag & ONLCR)) {
        for (s = 0; s < nsinks; s++) {
            if (sinks[s].enabled) {
                raw_write(sinks[s].dev, p, len);
            }
        }
        return;
    }

    for (i = 0; i <= len; i++) {
        if (i == len || p[i] == '\n') {
            if (i > start) {
                for (s = 0; s < nsinks; s++) {
                    if (sinks[s].enabled) {
                        raw_write(sinks[s].dev, p + start, i - start);
                    }
                }
            }
            if (i < len) {
                for (s = 0; s < nsinks; s++) {
                    if (sinks[s].enabled) {
                        raw_write(sinks[s].dev, "\r\n", 2);
                    }
                }
            }
            start = i + 1;
        }
    }
}

static s32 tty_write(struct file *f, const void *buf, u32 len)
{
    (void)f;
    sink_write(buf, len);
    return (s32)len;
}

static void echo(int c)
{
    u8 ch = (u8)c;

    if (tio.c_lflag & ECHO) {
        sink_write(&ch, 1);
    }
}

/* ---------------------------------------------------------------- */
/* Input                                                             */
/* ---------------------------------------------------------------- */

/* Anything waiting anywhere, without taking it. */
static int any_ready(void)
{
    int i;

    if (pushback >= 0) {
        return 1;
    }
    for (i = 0; i < nsources; i++) {
        if (source_ready(sources[i])) {
            return 1;
        }
    }
    return 0;
}

/* One character if there is one, or -1. Never waits. */
static int poll_char(void)
{
    int i;

    if (pushback >= 0) {
        int c = pushback;

        pushback = -1;
        return c;
    }
    /*
     * Round robin from the top each time rather than remembering where
     * it got to: with two or three sources the fairness question does
     * not arise, and a person typing on one of them is not racing
     * anybody.
     */
    for (i = 0; i < nsources; i++) {
        if (source_ready(sources[i])) {
            int c = source_get(sources[i]);

            if (c >= 0) {
                return c;
            }
        }
    }
    return -1;
}

/* One character, waiting for it. */
static int next_char(void)
{
    int c;

    reader_active = 1;
    for (;;) {
        c = poll_char();
        if (c >= 0) {
            break;
        }
        /*
         * Nothing waiting. There is no scheduler to yield to and no
         * interrupt to sleep on yet -- the UART's IRQ reaches MFP
         * channel 7 and is not enabled -- so this spins. It is the last
         * polling loop in the system and the one worth removing next.
         *
         * Until then it is also the system's idle time, and the network
         * uses it to process what has arrived.
         */
        if (idle_fn) {
            idle_fn();
        }
    }
    reader_active = 0;
    return c;
}

/*
 * Is this one of the characters that means something rather than being
 * one? Returns the signal it raised, or 0.
 *
 * The check is here and not in the two read paths because it has to
 * happen identically in both: a program that put the terminal in raw
 * mode to do its own line editing still expects ctrl-C to work, which
 * is exactly how a real terminal behaves -- readline clears ICANON and
 * ECHO and deliberately leaves ISIG alone.
 */
static int signal_char(int c)
{
    if (!(tio.c_lflag & ISIG)) {
        return 0;
    }
    if (tio.c_cc[VINTR] && c == tio.c_cc[VINTR]) {
        job_signal_fg(SIGINT);
        return SIGINT;
    }
    if (tio.c_cc[VSUSP] && c == tio.c_cc[VSUSP]) {
        job_signal_fg(SIGTSTP);
        return SIGTSTP;
    }
    return 0;
}

/*
 * Raw mode: whatever has arrived, as soon as anything has.
 *
 * VMIN 1 and VTIME 0, which is the setting a line editor uses and the
 * only one worth implementing without a scheduler to time out against.
 * More than one character comes back at a time when more than one is
 * waiting, so a paste does not cost a system call per byte.
 */
static s32 tty_read_raw(u8 *out, u32 len)
{
    u32 n = 0;

    for (;;) {
        int c = (n == 0) ? next_char() : poll_char();

        if (c < 0) {
            break;              /* nothing more waiting */
        }
        if (signal_char(c)) {
            /*
             * EINTR, exactly as a real read returns when a signal
             * arrives, and for the same reason: the caller has to find
             * out that something happened rather than sit waiting for a
             * character that was never meant as one.
             */
            return n > 0 ? (s32)n : -EINTR;
        }
        if (c == '\r' && (tio.c_iflag & ICRNL)) {
            c = '\n';
        }
        echo(c);
        out[n++] = (u8)c;
        if (n == len) {
            break;
        }
    }
    return (s32)n;
}

/*
 * Canonical mode: one line, echoed as it is typed, 0 at end of input.
 *
 * Echo goes to the sinks, not back to the source. That is the whole
 * reason this code moved out of the UART driver: what you type has to
 * appear on the screen you are looking at, which is not necessarily the
 * wire the character arrived on.
 */
static s32 tty_read_canon(u8 *out, u32 len)
{
    u32 n = 0;

    for (;;) {
        int c = next_char();

        if (signal_char(c)) {
            return -EINTR;
        }

        if (c == '\r' && (tio.c_iflag & ICRNL)) {
            c = '\n';
        }

        if (tio.c_cc[VEOF] && c == tio.c_cc[VEOF]) {
            /* End of input only on an empty line, as a real terminal
             * does it; mid-line it submits what has been typed. */
            return (s32)n;
        }

        if ((tio.c_cc[VERASE] && c == tio.c_cc[VERASE]) || c == '\b') {
            if (n > 0) {
                n--;
                if (tio.c_lflag & ECHO) {
                    sink_write("\b \b", 3);
                }
            }
            continue;
        }

        if (tio.c_cc[VKILL] && c == tio.c_cc[VKILL]) {
            while (n > 0) {
                n--;
                if (tio.c_lflag & ECHO) {
                    sink_write("\b \b", 3);
                }
            }
            continue;
        }

        if (c == '\n') {
            out[n++] = '\n';
            echo('\n');
            return (s32)n;
        }

        if (c < 32 || c > 126) {
            continue;                   /* other controls are not input */
        }

        if (n < len) {
            out[n++] = (u8)c;
            echo(c);
            if (n == len) {
                return (s32)n;          /* the caller's buffer is full */
            }
        }
        /* Otherwise refuse the character rather than overrun. */
    }
}

static s32 tty_read(struct file *f, void *buf, u32 len)
{
    (void)f;
    if (len == 0) {
        return 0;
    }
    if (tio.c_lflag & ICANON) {
        return tty_read_canon(buf, len);
    }
    return tty_read_raw(buf, len);
}

/* ---------------------------------------------------------------- */
/* The tick's look for a ctrl-C                                      */
/* ---------------------------------------------------------------- */

/*
 * Called from the timer interrupt while a program is running.
 *
 * A program that is reading will see its own ctrl-C and this never
 * runs. A program that is drawing a cube forever and reading nothing
 * would otherwise be uninterruptible, and on a machine with one console
 * that means the only way out is killing the emulator -- which is
 * precisely what ctrl-C is supposed to prevent.
 *
 * It takes at most one character per tick and pushes back anything that
 * is not a signal, so ordinary typed-ahead input survives untouched.
 */
void tty_poll_signals(void)
{
    int c;

    if (reader_active || pushback >= 0) {
        return;
    }
    if (!any_ready()) {
        return;
    }
    c = poll_char();
    if (c < 0) {
        return;
    }
    if (!signal_char(c)) {
        pushback = c;
    }
}

/* ---------------------------------------------------------------- */
/* ioctl                                                             */
/* ---------------------------------------------------------------- */

static int tty_ioctl(struct file *f, u32 request, u32 arg)
{
    (void)f;

    switch (request) {
    case FIONREAD:
        if (arg) {
            *(u32 *)arg = any_ready() ? 1 : 0;
        }
        return 0;

    /*
     * Which devices the console is made of, and turning one off.
     *
     * Here rather than in the shell because this is the only thing that
     * knows: the lists are private to this file, and a program asking
     * where its output goes should be asking the terminal, not reaching
     * into the kernel for a list it happens to be able to see.
     */
    case TIOCGCONS: {
        struct console_info *ci = (struct console_info *)arg;
        struct chardev *d;

        if (!ci) {
            return -EINVAL;
        }
        if (ci->which == CONS_SINK) {
            if (ci->index < 0 || ci->index >= nsinks) {
                return -ENOENT;
            }
            d = sinks[ci->index].dev;
            ci->enabled = sinks[ci->index].enabled;
        } else if (ci->which == CONS_SOURCE) {
            if (ci->index < 0 || ci->index >= nsources) {
                return -ENOENT;
            }
            d = sources[ci->index];
            /* A source is never off. Silently ignoring a keystroke
             * somebody typed is not a state worth being able to reach. */
            ci->enabled = 1;
        } else {
            return -EINVAL;
        }
        strncpy(ci->name, d->name, sizeof(ci->name) - 1);
        ci->name[sizeof(ci->name) - 1] = '\0';
        return 0;
    }

    case TIOCSCONS: {
        const struct console_set *cs = (const struct console_set *)arg;

        if (!cs) {
            return -EINVAL;
        }
        return tty_sink_enable(cs->name, cs->on);
    }

    case TCGETS:
        if (!arg) {
            return -EINVAL;
        }
        *(struct termios *)arg = tio;
        return 0;

    case TCSETSF:
        /* Throw away anything typed ahead, which is what the F is for:
         * a program switching modes does not want the previous mode's
         * leftovers interpreted under the new rules. */
        pushback = -1;
        while (any_ready()) {
            (void)poll_char();
        }
        /* fall through */
    case TCSETS:
    case TCSETSW:
        if (!arg) {
            return -EINVAL;
        }
        tio = *(const struct termios *)arg;
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

static const struct file_ops tty_ops = {
    tty_read,
    tty_write,
    0,                          /* a terminal is not seekable */
    tty_ioctl,
    tty_close
};

/* ---------------------------------------------------------------- */
/* Registration                                                      */
/* ---------------------------------------------------------------- */

int tty_add_source(struct chardev *d)
{
    if (!d || !d->ops || !d->ops->read) {
        return -EINVAL;
    }
    if (nsources == TTY_MAX_SOURCES) {
        return -ENOSPC;
    }
    sources[nsources++] = d;
    return 0;
}

int tty_add_sink(struct chardev *d)
{
    if (!d || !d->ops || !d->ops->write) {
        return -EINVAL;
    }
    if (nsinks == TTY_MAX_SINKS) {
        return -ENOSPC;
    }
    sinks[nsinks].dev = d;
    sinks[nsinks].enabled = 1;
    nsinks++;
    return 0;
}

int tty_sink_enable(const char *name, int on)
{
    int i, others = 0;

    for (i = 0; i < nsinks; i++) {
        if (strcmp(sinks[i].dev->name, name) != 0 && sinks[i].enabled) {
            others++;
        }
    }
    for (i = 0; i < nsinks; i++) {
        if (strcmp(sinks[i].dev->name, name) == 0) {
            /*
             * Refuse to turn off the last one. A machine with no console
             * output is one that cannot tell you why, and getting there
             * by typing one command would be an easy mistake to make.
             */
            if (!on && others == 0) {
                return -EBUSY;
            }
            sinks[i].enabled = on ? 1 : 0;
            return 0;
        }
    }
    return -ENODEV;
}

struct chardev *tty_sink(int index, int *enabled)
{
    if (index < 0 || index >= nsinks) {
        return 0;
    }
    if (enabled) {
        *enabled = sinks[index].enabled;
    }
    return sinks[index].dev;
}

struct chardev *tty_source(int index)
{
    if (index < 0 || index >= nsources) {
        return 0;
    }
    return sources[index];
}

struct chardev *tty_device(void)
{
    return &tty_dev;
}

/*
 * Put the terminal back the way a shell hands it to a program.
 *
 * Used after a program exits, because a program that set raw mode and
 * was then killed by ctrl-C never got the chance to put it back -- and a
 * console left in raw mode with echo off looks exactly like a machine
 * that has crashed.
 */
void tty_reset(void)
{
    tio.c_iflag = ICRNL;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_lflag = ISIG | ICANON | ECHO;
}

int tty_init(void)
{
    int err;

    tty_dev.name = "console";
    tty_dev.ops = &tty_ops;
    tty_dev.priv = 0;
    tty_dev.next = 0;

    err = dev_register_char(&tty_dev);
    if (err < 0) {
        return err;
    }

    /* A second name for the same terminal. They differ on a real system;
     * here there is one, and pretending otherwise would be a lie with no
     * payoff. */
    tty_alias.name = "tty";
    tty_alias.ops = &tty_ops;
    tty_alias.priv = 0;
    tty_alias.next = 0;
    dev_register_char(&tty_alias);

    console_set(&tty_dev);

    /* Descriptors 0, 1 and 2 are the terminal, bound before anything can
     * try to use them, exactly as a shell would inherit them. They never
     * move again: what changes is which sinks the terminal writes to. */
    fd_bind(0, &tty_ops, 0, O_RDONLY);
    fd_bind(1, &tty_ops, 0, O_WRONLY);
    fd_bind(2, &tty_ops, 0, O_WRONLY);
    return 0;
}
