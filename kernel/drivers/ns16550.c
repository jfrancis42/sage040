/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ns16550.c - the serial port, an NS16550A at 0xff000000.
 *
 * Reference: TI/National PC16550D datasheet. Registers are byte-spaced,
 * so register N of the datasheet is byte N of the window.
 *
 * A serial port and nothing more: bytes in, bytes out, no translation
 * and no line editing. It registers as /dev/ttyS0 and hands itself to
 * the terminal layer as both a source and a sink.
 *
 * The line discipline used to live here, which was fine while a UART was
 * the only thing a character could arrive on or appear at. It is in
 * tty.c now -- see the comment there for why that had to move once
 * there was a screen as well.
 *
 * Polled in both directions. That is a starting point chosen on purpose:
 * a polled port works before interrupts are set up, works inside a
 * panic, and cannot deadlock against the code reporting the fault. The
 * chip's interrupt reaches MFP channel 7 and is the obvious next thing
 * to use -- feeding a ring buffer that tty.c drains, which would remove
 * the last polling loop in the system.
 */
#include "dev.h"
#include "tty.h"
#include "errno.h"
#include "vfs.h"
#include "drivers.h"

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

/* ---------------------------------------------------------------- */
/* file_ops -- raw, and deliberately so                              */
/* ---------------------------------------------------------------- */

static s32 serial_write(struct file *f, const void *buf, u32 len)
{
    const u8 *p = buf;
    u32 i;

    (void)f;
    for (i = 0; i < len; i++) {
        uart_putc_raw((char)p[i]);
    }
    return (s32)len;
}

/*
 * Up to len bytes, blocking until at least one arrives.
 *
 * No line editing, no echo and no end-of-input handling: those are the
 * terminal's, and tty.c only ever asks for one byte at a time anyway,
 * having first asked FIONREAD whether there is one.
 */
static s32 serial_read(struct file *f, void *buf, u32 len)
{
    u8 *out = buf;
    u32 n = 0;

    (void)f;
    if (len == 0) {
        return 0;
    }
    while (!uart_rx_ready()) {
        /* spin until a character arrives */
    }
    while (n < len && uart_rx_ready()) {
        out[n++] = MMIO8(UART_RBR);
    }
    return (s32)n;
}

static int serial_ioctl(struct file *f, u32 request, u32 arg)
{
    (void)f;

    switch (request) {
    case FIONREAD:
        /*
         * How many bytes could be read without blocking. Nothing is
         * buffered here, so the honest answer is 1 if the receiver holds
         * a character and 0 if it does not -- there may be more behind
         * it in the chip's FIFO, and this does not claim otherwise.
         *
         * This is what makes the port usable as a terminal input source:
         * it is how tty.c asks whether there is anything to take without
         * committing to a read that would block until there is.
         */
        if (arg) {
            *(u32 *)arg = uart_rx_ready() ? 1 : 0;
        }
        return 0;

    default:
        return -ENOTTY;
    }
}

static int serial_close(struct file *f)
{
    (void)f;
    return 0;                   /* the port outlives every descriptor */
}


/*
 * A character device has no size and no meaningful time; what a caller
 * actually wants from this is S_ISCHR, which is how isatty() is built.
 */
static int serial_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR;
    st->st_size = 0;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static const struct file_ops serial_ops = {
    serial_read,
    serial_write,
    0,                          /* a serial port is not seekable */
    serial_ioctl,
    serial_close,
    serial_fstat,
    0,                          /* poll: the default; see dev.h */
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

static struct chardev serial_dev = { .name = "ttyS0", .ops = &serial_ops };

/* ---------------------------------------------------------------- */

/*
 * Is the chip actually there? The scratch register is the standard way
 * to tell: it is the one register with no side effects.
 */
int ns16550_present(void)
{
    /* Is the chip fitted? An address with nothing behind it raises a
     * bus error rather than reading back zeroes, so this has to be
     * asked before the first register access, not by making one. */
    if (!io_probe8((volatile void *)UART_BASE)) {
        return 0;
    }

    u8 saved = MMIO8(UART_SCR);
    int ok;

    MMIO8(UART_SCR) = 0xa5;
    ok = (MMIO8(UART_SCR) == 0xa5);
    MMIO8(UART_SCR) = 0x5a;
    ok = ok && (MMIO8(UART_SCR) == 0x5a);
    MMIO8(UART_SCR) = saved;
    return ok;
}

#define IER_RDA     0x01        /* received data available */

static void serial_isr(void *arg)
{
    (void)arg;
    tty_input_irq();
    /* Reading RBR until LSR says empty is what drops INT; the IIR read
     * is for form -- it is how a real driver learns why, and the chip
     * expects it. */
    (void)MMIO8(UART_IIR);
}

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

    err = dev_register_char(&serial_dev);
    if (err < 0) {
        return err;
    }

    /*
     * Both, and always. The serial line is the one thing on this machine
     * that works before anything else is up and keeps working whatever
     * the display is doing, so it is the console of last resort and is
     * never removed from either list.
     */
    tty_add_source(&serial_dev);
    tty_add_sink(&serial_dev);
    return 0;
}

/*
 * The receive interrupt (task 22): data-available only. The chip's INT
 * output is wired to MFP GPIP5; the handler drains the FIFO into the
 * terminal's ring.
 *
 * Separate from ns16550_init() because the serial port comes up FIRST
 * -- it is the console, before anything else can report anything -- and
 * the MFP a good while later, and mfp_init() clears every handler and
 * enable it finds. So this is called after it, from start_drivers().
 * Enabled only once the handler is in place: an interrupt with nothing
 * to answer it storms.
 */
int ns16550_irq_on(void)
{
    int err = mfp_request_gpip(MFP_PIN_UART, serial_isr, 0);

    if (err < 0) {
        return err;             /* and the port stays polled */
    }
    MMIO8(UART_IER) = IER_RDA;
    return tty_source_irq(&serial_dev);
}

struct chardev *ns16550_device(void)
{
    return &serial_dev;
}
