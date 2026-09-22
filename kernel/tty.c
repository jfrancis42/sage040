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

/*
 * INTERRUPT-DRIVEN INPUT (task 22). A source whose driver takes its
 * chip's receive interrupt is marked here, and its interrupt handler
 * calls tty_input_irq(), which drains the chip into this ring -- acting
 * on ctrl-C and ctrl-Z as it goes -- and wakes whoever is waiting. Such
 * a source is never polled: the ring is where its characters are.
 *
 * Sources without an interrupt (the console's own replies, fbcon) are
 * polled as before, from the reader and from the tick.
 */
#define IN_RING 256

static u8  in_ring[IN_RING];
static u32 in_head, in_tail;            /* written at head, read at tail */
static u32 in_overruns;
static int in_stalled;                  /* left in the chip: ring full */
static u8  source_irq[TTY_MAX_SOURCES];

static int ring_empty(void)
{
    return in_head == in_tail;
}

/*
 * Who ctrl-C and ctrl-Z are aimed at: the foreground PROCESS GROUP.
 *
 * A group, not a task, because a pipeline is several programs and the
 * key has to reach all of them. The shell puts each job in a group of
 * its own and hands the terminal to that group while it waits. A job in
 * the background is in a group that does not have the terminal, which
 * is the whole difference between running something with & and without
 * -- and a background task that tries to READ is sent SIGTTIN, so it
 * stops instead of taking keystrokes meant for somebody else.
 */
static int fg_pgrp;

/*
 * A ctrl-C or ctrl-Z typed while the foreground group has no program in
 * it -- the shell is between reading a line and handing the terminal to
 * the job it started. Starting a program sleeps now (the disk is
 * interrupt-driven), so the stages of a pipeline already started run,
 * and print, while the shell is still starting the rest; a person who
 * sees that and types ctrl-C expects it to reach them. It used to go to
 * the shell's own group, reach nobody, and leave the shell waiting for
 * a pipeline nothing would end. Now it is kept, and delivered to the
 * group the terminal is handed to next -- by then every stage exists,
 * which is why it is not simply given to the group as it grows.
 *
 * The ctrl-C character itself also goes in the input ring, and whoever
 * READS it -- the shell's line editor, when the shell was at its prompt
 * -- clears this (signal_char): a key typed at a prompt must not kill
 * the next command.
 */
static int pending_sig;

void tty_set_foreground(int pgrp)
{
    int sig = pending_sig;

    fg_pgrp = pgrp;
    pending_sig = 0;
    if (sig && pgrp && (!current || pgrp != current->pgid)) {
        signal_group(pgrp, sig);
    }
}

int tty_foreground(void)
{
    return fg_pgrp;
}

/*
 * May the current task read the terminal? 0 if so. A task outside the
 * foreground group is sent SIGTTIN, whose default is to stop it; the
 * read returns -EINTR, and when `fg` continues it the read is restarted
 * as if nothing had happened (see signal.c). A task that ignores or
 * blocks SIGTTIN would never be stopped, so it gets EIO instead --
 * POSIX's rule, and what stops such a task spinning on the signal.
 */
static int may_read(void)
{
    struct task *t = current;

    if (!t || !t->as || t->pgid == fg_pgrp) {
        return 0;
    }
    if ((t->sig_blocked & SIGMASK(SIGTTIN)) ||
        t->sigact[SIGTTIN].sa_handler == SIG_IGN) {
        return -EIO;
    }
    signal_group(t->pgid, SIGTTIN);
    return -EINTR;
}

/*
 * Anything blocked waiting for a keystroke.
 *
 * This is what replaced the spin. The terminal's read used to go round a
 * loop asking the UART whether anything had arrived, which was fine when
 * there was nothing else for the processor to do -- and is not, now that
 * there is.
 */
static struct waitq input_wait;

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

    if (pushback >= 0 || !ring_empty()) {
        return 1;
    }
    for (i = 0; i < nsources; i++) {
        if (!source_irq[i] && source_ready(sources[i])) {
            return 1;
        }
    }
    return 0;
}

/*
 * One character if there is one, or -1. Never waits.
 *
 * MASKED, and that is not caution -- it is a bug that was observed.
 * The timer interrupt runs tty_poll_signals(), which also takes a
 * character and may put it in the pushback slot. If it lands between a
 * reader checking that slot and the same reader taking one from the
 * device, the reader gets the LATER character and the earlier one waits
 * in pushback for the next call. The two come out in the wrong order --
 * a line typed as SHELL arrives as SHLEL, once in a few thousand
 * characters, which is exactly often enough to be baffling.
 */
void tty_input_irq(void);

static int poll_char(void)
{
    u16 sr = irq_save();
    int c = -1;
    int i;

    if (pushback >= 0) {
        c = pushback;
        pushback = -1;
        irq_restore(sr);
        return c;
    }
    if (!ring_empty()) {
        c = in_ring[in_tail % IN_RING];
        in_tail++;
        /* Room again: take what was left waiting in the chip, which
         * will not interrupt again until it has been emptied. */
        if (in_stalled && in_head - in_tail < IN_RING / 2) {
            in_stalled = 0;
            tty_input_irq();
        }
        irq_restore(sr);
        return c;
    }
    /*
     * Round robin from the top each time rather than remembering where
     * it got to: with two or three sources the fairness question does
     * not arise, and a person typing on one of them is not racing
     * anybody.
     */
    for (i = 0; i < nsources; i++) {
        if (!source_irq[i] && source_ready(sources[i])) {
            c = source_get(sources[i]);

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
 * One character, waiting for it.
 *
 * Sleeps rather than spins. The UART and the keyboard are still POLLED
 * -- neither interrupt is enabled -- so what wakes this is the timer,
 * through tty_poll_signals(); the saving is not in how the character is
 * noticed but in what the processor does while there is none, which is
 * now "run something else" rather than "ask again".
 *
 * Returns -EINTR if a signal arrived instead of a character, which is
 * what makes ctrl-C reach a program blocked on a read.
 */
static int next_char(void)
{
    for (;;) {
        int c = poll_char();

        if (c >= 0) {
            return c;
        }
        if (signal_pending(current)) {
            return -EINTR;
        }
        /* A tenth of a second, so that a lost wakeup costs a small
         * delay rather than a hang. */
        sleep_on_timeout(&input_wait, 100);
    }
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
/* The signal this character means, if it means one; 0 if not. */
static int signal_of(int c)
{
    if (!(tio.c_lflag & ISIG)) {
        return 0;
    }
    if (tio.c_cc[VINTR] && c == tio.c_cc[VINTR]) {
        return SIGINT;
    }
    if (tio.c_cc[VSUSP] && c == tio.c_cc[VSUSP]) {
        return SIGTSTP;
    }
    return 0;
}

static int signal_char(int c)
{
    int sig = signal_of(c);

    if (!sig) {
        return 0;
    }

    /*
     * To the foreground task, whoever that is. A background task is
     * unaffected -- it is not connected to this keyboard in the sense
     * that matters -- and that is the entire semantic difference between
     * a job started with & and one without.
     *
     * Called from a READER, which is what has now dealt with the key:
     * nothing is left pending for anybody else (see pending_sig).
     */
    signal_group(fg_pgrp, sig);
    pending_sig = 0;
    return sig;
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
        if (c == -EINTR) {
            return n > 0 ? (s32)n : -EINTR;
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

        if (c == -EINTR || signal_char(c)) {
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

        if (c < 32 || c == 127) {
            continue;                   /* other controls are not input;
                                         * bytes above 127 are UTF-8 */
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
    int err;

    (void)f;
    if (len == 0) {
        return 0;
    }
    err = may_read();
    if (err < 0) {
        return err;
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
/*
 * Called from the timer interrupt.
 *
 * Two jobs. It looks for an interrupt or stop character, so that a
 * program making no system calls can still be stopped -- and it wakes
 * anything sleeping for input, because the UART and the keyboard are
 * polled and nothing else would.
 */
/*
 * Called from a source's receive interrupt. Takes EVERYTHING waiting
 * from every interrupt-driven source: the chip's interrupt input is an
 * edge on the MFP, and a character left behind would keep the line high
 * and never make another. ctrl-C and ctrl-Z are acted on here, at once,
 * rather than at the next tick; everything else goes in the ring.
 */
void tty_input_irq(void)
{
    int i;
    int got = 0;

    for (i = 0; i < nsources; i++) {
        struct chardev *d = sources[i];
        int n;

        if (!source_irq[i]) {
            continue;
        }
        for (n = 0; n < 4096 && source_ready(d); n++) {
            int c;

            /*
             * FULL: stop, and leave the rest in the chip. The chip's
             * FIFO filling is what makes the sender wait -- QEMU's
             * serial model will not deliver into a full FIFO, and a
             * real line has flow control for exactly this -- whereas a
             * character taken and then dropped is gone. The line stays
             * high, so no edge will come: poll_char() resumes the drain
             * when a reader makes room.
             */
            if (in_head - in_tail >= IN_RING) {
                in_stalled = 1;
                break;
            }
            c = source_get(d);
            if (c < 0) {
                break;
            }
            /*
             * ctrl-C and ctrl-Z act NOW when there is a program to act
             * on. When there is not -- the foreground group is the
             * shell's -- the character goes in the ring for whoever
             * reads next, as it would have been read before input was
             * interrupt-driven: the shell's line editor sees it and
             * abandons its line. It is also kept as pending_sig, in
             * case what reads next is not the shell but the terminal
             * being handed to a job the shell was starting.
             */
            {
                int sig = signal_of(c);

                if (sig) {
                    if (signal_group(fg_pgrp, sig) == 0) {
                        continue;
                    }
                    pending_sig = sig;
                }
            }
            in_ring[in_head % IN_RING] = (u8)c;
            in_head++;
            got = 1;
        }
    }
    if (got) {
        wake_all(&input_wait);
        poll_wake();
    }
}

void tty_poll_signals(void)
{
    int c;

    /*
     * Only the sources with no interrupt: the ring is already a reader's
     * business, and taking a ctrl-C out of it here would swallow one
     * the shell's editor was meant to see.
     */
    if (pushback < 0) {
        u16 sr = irq_save();
        int i;

        c = -1;
        for (i = 0; i < nsources && c < 0; i++) {
            if (!source_irq[i] && source_ready(sources[i])) {
                c = source_get(sources[i]);
            }
        }
        irq_restore(sr);
        if (c >= 0 && !signal_char(c)) {
            pushback = c;
        }
    }

    if (pushback >= 0 || any_ready()) {
        wake_all(&input_wait);
        poll_wake();            /* a program in poll() or select() too */
    }
}

/* ---------------------------------------------------------------- */
/* ioctl                                                             */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Size                                                              */
/* ---------------------------------------------------------------- */

/*
 * The serial line's size. A terminal on a wire cannot say how big it
 * is, so it is a VT100's 24x80 until TIOCSWINSZ -- `stty rows`, or
 * `resize`, which asks the terminal -- says otherwise.
 */
static struct winsize line_ws = { 24, 80, 0, 0 };

/*
 * What a program is told: the smallest of the enabled outputs, because
 * a full-screen program has to fit on every one of them at once. A sink
 * that knows its own size answers TIOCGWINSZ (the screen does); one
 * that does not counts as the line.
 */
static void effective_ws(struct winsize *out)
{
    struct winsize w[TTY_MAX_SINKS], best;
    int i, n = 0;

    for (i = 0; i < nsinks; i++) {
        struct file f;

        if (!sinks[i].enabled) {
            continue;
        }
        as_file(&f, sinks[i].dev);
        memset(&w[n], 0, sizeof(w[n]));
        if (!sinks[i].dev->ops->ioctl ||
            sinks[i].dev->ops->ioctl(&f, TIOCGWINSZ, (u32)&w[n]) < 0 ||
            w[n].ws_row == 0 || w[n].ws_col == 0) {
            w[n] = line_ws;
        }
        n++;
    }
    if (n == 0) {
        *out = line_ws;
        return;
    }

    best = w[0];
    for (i = 1; i < n; i++) {
        if (w[i].ws_row < best.ws_row) {
            best.ws_row = w[i].ws_row;
        }
        if (w[i].ws_col < best.ws_col) {
            best.ws_col = w[i].ws_col;
        }
    }
    /* Pixels only when one output is exactly the size chosen; a
     * mixture of two outputs' dimensions is not the size of anything. */
    best.ws_xpixel = best.ws_ypixel = 0;
    for (i = 0; i < n; i++) {
        if (w[i].ws_row == best.ws_row && w[i].ws_col == best.ws_col) {
            best.ws_xpixel = w[i].ws_xpixel;
            best.ws_ypixel = w[i].ws_ypixel;
            break;
        }
    }
    *out = best;
}

/* Tell the foreground if something just changed what it would be told. */
static void winch_if_changed(const struct winsize *before)
{
    struct winsize after;

    effective_ws(&after);
    if (after.ws_row != before->ws_row || after.ws_col != before->ws_col) {
        signal_group(fg_pgrp, SIGWINCH);
    }
}

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
    /*
     * The foreground group, as tcgetpgrp() and tcsetpgrp() see it. A
     * group must have a member to be given the terminal; handing it to
     * nobody would leave ctrl-C going nowhere.
     */
    case TIOCGPGRP:
        *(int *)arg = fg_pgrp;
        return 0;

    case TIOCSPGRP: {
        int pg = *(int *)arg;
        struct task *t;
        int i;

        for (i = 0; (t = task_nth(i)) != 0; i++) {
            if (t->pgid == pg && t->state != TASK_ZOMBIE) {
                fg_pgrp = pg;
                return 0;
            }
        }
        return -EPERM;
    }

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
        struct winsize before;
        int err;

        if (!cs) {
            return -EINVAL;
        }
        effective_ws(&before);
        err = tty_sink_enable(cs->name, cs->on);
        if (err == 0) {
            winch_if_changed(&before);
        }
        return err;
    }

    case TIOCGWINSZ:
        if (!arg) {
            return -EINVAL;
        }
        effective_ws((struct winsize *)arg);
        return 0;

    case TIOCSWINSZ: {
        struct winsize before;

        if (!arg) {
            return -EINVAL;
        }
        effective_ws(&before);
        line_ws = *(const struct winsize *)arg;
        winch_if_changed(&before);
        return 0;
    }

    case TCGETS:
        if (!arg) {
            return -EINVAL;
        }
        *(struct termios *)arg = tio;
        return 0;

    /* The termios2 forms: the same settings, plus two speeds that are
     * reported as 38400 and not changed by setting them. */
    case TCGETS2: {
        struct termios2 *t2 = (struct termios2 *)arg;

        if (!t2) {
            return -EINVAL;
        }
        memcpy(t2, &tio, sizeof(tio));
        t2->c_ispeed = t2->c_ospeed = 38400;
        return 0;
    }

    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2: {
        struct termios t;

        if (!arg) {
            return -EINVAL;
        }
        memcpy(&t, (const void *)arg, sizeof(t));
        return tty_ioctl(f, request == TCSETSF2 ? TCSETSF : TCSETS, (u32)&t);
    }

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


/*
 * A character device has no size and no meaningful time; what a caller
 * actually wants from this is S_ISCHR, which is how isatty() is built.
 */
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
    tty_read,
    tty_write,
    0,                          /* a terminal is not seekable */
    tty_ioctl,
    tty_close,
    tty_fstat,
    0,                          /* poll: the default; see dev.h */
    0,                          /* truncate: nothing to truncate */
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
    source_irq[nsources] = 0;
    sources[nsources++] = d;
    return 0;
}

int tty_source_irq(struct chardev *d)
{
    int i;

    for (i = 0; i < nsources; i++) {
        if (sources[i] == d) {
            source_irq[i] = 1;
            /* Anything that arrived before the interrupt was on would
             * hold the line high and never make an edge: take it now. */
            {
                u16 sr = irq_save();

                tty_input_irq();
                irq_restore(sr);
            }
            return 0;
        }
    }
    return -ENOENT;
}

u32 tty_overruns(void)
{
    return in_overruns;
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
