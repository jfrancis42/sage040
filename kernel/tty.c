/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * tty.c - the line discipline, and the fan-out.
 *
 * This used to live inside the NS16550A driver, which worked for exactly
 * as long as there was one place characters could come from and one
 * place they could go. There is now a screen as well as a serial port,
 * and a keyboard is next, so it moves out to where it belongs.
 *
 * What a terminal does here:
 *
 *   read()   assembles one line, echoing as it goes, with backspace,
 *            ctrl-U and ctrl-D doing what a person expects. Canonical
 *            mode. Characters are taken from whichever source has one.
 *
 *   write()  turns a newline into carriage return and newline, and sends
 *            the result to every enabled sink.
 *
 * Both translations are named after the termios flags that do the same
 * job on a real system -- ONLCR on the way out, ICRNL on the way in --
 * and both belong here rather than in a driver, because they are
 * properties of a terminal and not of a chip.
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
#include "console.h"
#include "errno.h"
#include "string.h"

#define CTRL_D     0x04
#define CTRL_U     0x15
#define BACKSPACE  0x08
#define DEL        0x7f

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

/* ---------------------------------------------------------------- */
/* Input                                                             */
/* ---------------------------------------------------------------- */

/*
 * One character from whichever source has one.
 *
 * Round robin from the top each time rather than remembering where it
 * got to: with two or three sources the fairness question does not
 * arise, and a person typing on one of them is not racing anybody.
 */
static int next_char(void)
{
    for (;;) {
        int i;

        for (i = 0; i < nsources; i++) {
            if (source_ready(sources[i])) {
                int c = source_get(sources[i]);

                if (c >= 0) {
                    return c;
                }
            }
        }
        /*
         * Nothing waiting. There is no scheduler to yield to and no
         * interrupt to sleep on yet -- the UART's IRQ reaches MFP
         * channel 7 and is not enabled -- so this spins. It is the last
         * polling loop in the system and the one worth removing next.
         */
    }
}

/*
 * Canonical mode: one line, echoed as it is typed, 0 at end of input.
 *
 * Echo goes to the sinks, not back to the source. That is the whole
 * reason this code moved out of the UART driver: what you type has to
 * appear on the screen you are looking at, which is not necessarily the
 * wire the character arrived on.
 */
static s32 tty_read(struct file *f, void *buf, u32 len)
{
    u8 *out = buf;
    u32 n = 0;

    (void)f;
    if (len == 0) {
        return 0;
    }

    for (;;) {
        int c = next_char();

        if (c == '\r') {
            c = '\n';                   /* ICRNL */
        }

        if (c == CTRL_D) {
            /* End of input only on an empty line, as a real terminal
             * does it; mid-line it submits what has been typed. */
            return (s32)n;
        }

        if (c == BACKSPACE || c == DEL) {
            if (n > 0) {
                n--;
                sink_write("\b \b", 3);
            }
            continue;
        }

        if (c == CTRL_U) {
            while (n > 0) {
                n--;
                sink_write("\b \b", 3);
            }
            continue;
        }

        if (c == '\n') {
            out[n++] = '\n';
            sink_write("\n", 1);
            return (s32)n;
        }

        if (c < 32 || c > 126) {
            continue;                   /* other controls are not input */
        }

        if (n < len) {
            out[n++] = (u8)c;
            sink_write(&out[n - 1], 1);
            if (n == len) {
                return (s32)n;          /* the caller's buffer is full */
            }
        }
        /* Otherwise refuse the character rather than overrun. */
    }
}

static int tty_ioctl(struct file *f, u32 request, u32 arg)
{
    (void)f;

    switch (request) {
    case FIONREAD: {
        int i;
        u32 n = 0;

        /* Anything waiting on any source counts. */
        for (i = 0; i < nsources; i++) {
            if (source_ready(sources[i])) {
                n = 1;
                break;
            }
        }
        if (arg) {
            *(u32 *)arg = n;
        }
        return 0;
    }

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
