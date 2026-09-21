/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ns16550.c - the console terminal, an NS16550A at 0xff000000.
 *
 * Reference: TI/National PC16550D datasheet. Registers are byte-spaced,
 * so register N of the datasheet is byte N of the window.
 *
 * This is a terminal, not just a UART, and the difference is the line
 * discipline in tty_read(): a read returns one whole line, echoed as it
 * is typed, with backspace and ctrl-U doing what a person expects and
 * ctrl-D ending the input. That is canonical mode, and it belongs here
 * for the same reason it belongs in the tty layer on a real system --
 * every program that reads a line would otherwise implement it again,
 * slightly differently.
 *
 * Two translations, both named after the termios flags that do the same
 * job on a real system:
 *
 *   ONLCR  a newline written out becomes carriage return + newline,
 *          because a terminal needs both and nothing else does. Files
 *          written through the same write() get the bare newline they
 *          should have.
 *   ICRNL  the carriage return the terminal sends when you press enter
 *          arrives as a newline, because that is what C code expects to
 *          find at the end of a line.
 *
 * Echo goes to whichever device is the console, not to this one. With
 * the console moved to the framebuffer, characters still arrive here --
 * the serial line is the only thing on this machine that can type -- but
 * they have to appear on the screen being looked at.
 *
 * Polled in both directions. That is a starting point chosen on purpose:
 * a polled console works before interrupts are set up, works inside a
 * panic, and cannot deadlock against the code reporting the fault. The
 * chip's interrupt already reaches MFP channel 7, and moving to it means
 * changing tty_read() to take from a ring buffer -- nothing above this
 * file moves.
 */
#include "dev.h"
#include "console.h"
#include "errno.h"
#include "vfs.h"

#define CTRL_D      0x04
#define CTRL_U      0x15
#define BACKSPACE   0x08
#define DEL         0x7f

static void uart_putc_raw(char c)
{
    while (!(MMIO8(UART_LSR) & LSR_THRE)) {
        /* wait for the transmit holding register to empty */
    }
    MMIO8(UART_THR) = (u8)c;
}

static int uart_rx_ready(void)
{
    return (MMIO8(UART_LSR) & LSR_DR) != 0;
}

static u8 uart_getc_raw(void)
{
    while (!uart_rx_ready()) {
        /* spin until a character arrives */
    }
    return MMIO8(UART_RBR);
}

/* ---------------------------------------------------------------- */
/* file_ops                                                          */
/* ---------------------------------------------------------------- */

static s32 tty_write(struct file *f, const void *buf, u32 len)
{
    const u8 *p = buf;
    u32 i;

    (void)f;
    for (i = 0; i < len; i++) {
        if (p[i] == '\n') {
            uart_putc_raw('\r');        /* ONLCR */
        }
        uart_putc_raw((char)p[i]);
    }
    return (s32)len;
}

/*
 * Canonical-mode read: one line, or fewer bytes if the caller's buffer
 * fills first. Returns 0 at end of input, which is what ctrl-D on an
 * empty line means and is how a program knows to stop.
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
        int c = uart_getc_raw();

        if (c == '\r') {
            c = '\n';                   /* ICRNL */
        }

        if (c == CTRL_D) {
            /* End of input only on an empty line, as a real terminal
             * does it; mid-line it submits what has been typed. */
            if (n == 0) {
                return 0;
            }
            return (s32)n;
        }

        if (c == BACKSPACE || c == DEL) {
            if (n > 0) {
                n--;
                console_write("\b \b", 3);
            }
            continue;
        }

        if (c == CTRL_U) {
            while (n > 0) {
                n--;
                console_write("\b \b", 3);
            }
            continue;
        }

        if (c == '\n') {
            out[n++] = '\n';
            console_write("\n", 1);
            return (s32)n;
        }

        if (c < 32 || c > 126) {
            continue;                   /* ignore the rest of the controls */
        }

        if (n < len) {
            out[n++] = (u8)c;
            console_write(&out[n - 1], 1);
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
    case FIONREAD:
        /*
         * How many bytes could be read without blocking. Nothing is
         * buffered here, so the honest answer is 1 if the receiver holds
         * a character and 0 if it does not -- there may be more behind it
         * in the chip's FIFO, and this does not claim otherwise.
         *
         * It exists so a program can ask "has a key been pressed" without
         * committing to a read that would block until one is. That is the
         * whole reason the cube can be stopped.
         */
        if (arg) {
            *(u32 *)arg = uart_rx_ready() ? 1 : 0;
        }
        return 0;

    default:
        return -ENOTTY;
    }
}

static int tty_close(struct file *f)
{
    (void)f;
    return 0;                   /* the console outlives every descriptor */
}

static const struct file_ops tty_ops = {
    tty_read,
    tty_write,
    0,                          /* no lseek: a terminal is not seekable */
    tty_ioctl,
    tty_close
};

static struct chardev tty_dev = {
    "console",
    &tty_ops,
    0,
    0
};

/*
 * A second name for the same device, so that both /dev/console and
 * /dev/tty work. They differ on a real system; here there is one
 * terminal and pretending otherwise would be a lie with no payoff.
 */
static struct chardev tty_alias = {
    "tty",
    &tty_ops,
    0,
    0
};

/* ---------------------------------------------------------------- */

int ns16550_init(void)
{
    int err;

    /*
     * 8N1, FIFOs enabled and cleared, no interrupts, DTR and RTS
     * asserted. The divisor is programmed for form's sake -- baud rate
     * means nothing to an emulated UART, but a real one needs it and the
     * sequence should be the one real hardware wants.
     */
    MMIO8(UART_IER) = 0x00;
    MMIO8(UART_LCR) = LCR_DLAB;
    MMIO8(UART_DLL) = 0x01;
    MMIO8(UART_DLM) = 0x00;
    MMIO8(UART_LCR) = LCR_8N1;
    MMIO8(UART_FCR) = FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX;
    MMIO8(UART_MCR) = MCR_DTR | MCR_RTS;

    err = dev_register_char(&tty_dev);
    if (err < 0) {
        return err;
    }
    dev_register_char(&tty_alias);

    console_set(&tty_dev);

    /* Descriptors 0, 1 and 2 are the terminal, bound before anything can
     * try to use them, exactly as a shell would inherit them. */
    fd_bind(0, &tty_ops, 0, O_RDONLY);
    fd_bind(1, &tty_ops, 0, O_WRONLY);
    fd_bind(2, &tty_ops, 0, O_WRONLY);
    return 0;
}

/*
 * Is the chip actually there? The scratch register is the standard way
 * to tell: it is the one register with no side effects.
 */
int ns16550_present(void)
{
    u8 saved = MMIO8(UART_SCR);
    int ok;

    MMIO8(UART_SCR) = 0xa5;
    ok = (MMIO8(UART_SCR) == 0xa5);
    MMIO8(UART_SCR) = 0x5a;
    ok = ok && (MMIO8(UART_SCR) == 0x5a);
    MMIO8(UART_SCR) = saved;
    return ok;
}
