/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * console.c - the kernel's console, an NS16550A at 0xff000000.
 *
 * Reference: TI/National PC16550D datasheet.  Registers are byte-spaced,
 * so register N of the datasheet is byte N of the window.
 *
 * Polled in both directions.  That is a deliberate starting point, not an
 * oversight: a polled console works before interrupts are set up, works
 * inside a panic, and cannot deadlock against the code reporting the
 * fault.  When the receive path becomes interrupt-driven (the UART's IRQ
 * already reaches MFP channel 7) kgetc() will take from a ring buffer
 * instead of the line status register, and nothing above it changes.
 *
 * This is the kernel's own driver rather than a call into ../tests/uart.c.
 * The tests share that file so their output is uniform; the kernel owns
 * its hardware.
 */
#include "kernel.h"

void con_init(void)
{
    /*
     * 8N1, FIFOs enabled and cleared, no interrupts, DTR and RTS
     * asserted.  The divisor is programmed for form's sake -- baud rate
     * is meaningless to an emulated UART, but a real one needs it and
     * the sequence should be the one real hardware wants.
     */
    MMIO8(UART_IER) = 0x00;
    MMIO8(UART_LCR) = LCR_DLAB;
    MMIO8(UART_DLL) = 0x01;
    MMIO8(UART_DLM) = 0x00;
    MMIO8(UART_LCR) = LCR_8N1;
    MMIO8(UART_FCR) = FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX;
    MMIO8(UART_MCR) = MCR_DTR | MCR_RTS;
}

/* ---------------------------------------------------------------- */
/* Output                                                            */
/* ---------------------------------------------------------------- */

void kputc(char c)
{
    if (c == '\n') {
        while (!(MMIO8(UART_LSR) & LSR_THRE)) {
            /* wait for the transmit holding register */
        }
        MMIO8(UART_THR) = '\r';
    }
    while (!(MMIO8(UART_LSR) & LSR_THRE)) {
        /* wait for the transmit holding register */
    }
    MMIO8(UART_THR) = (u8)c;
}

void kputs(const char *s)
{
    while (*s) {
        kputc(*s++);
    }
}

void kputln(const char *s)
{
    kputs(s);
    kputc('\n');
}

static const char hexdigits[] = "0123456789abcdef";

void kputhex8(u8 v)
{
    kputc(hexdigits[(v >> 4) & 0xf]);
    kputc(hexdigits[v & 0xf]);
}

void kputhex16(u16 v)
{
    kputhex8((u8)(v >> 8));
    kputhex8((u8)v);
}

void kputhex32(u32 v)
{
    kputhex16((u16)(v >> 16));
    kputhex16((u16)v);
}

void kputdec(u32 v)
{
    char buf[12];
    int i = 0;

    if (v == 0) {
        kputc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        kputc(buf[--i]);
    }
}

/* ---------------------------------------------------------------- */
/* Input                                                             */
/* ---------------------------------------------------------------- */

int kgetc_nb(void)
{
    if (MMIO8(UART_LSR) & LSR_DR) {
        return (int)MMIO8(UART_RBR);
    }
    return -1;
}

int kgetc(void)
{
    int c;

    while ((c = kgetc_nb()) < 0) {
        /* spin until a character arrives */
    }
    return c;
}

/*
 * Read one line into buf, echoing as it goes, and return its length.
 *
 * Editing is what a serial terminal can do without cursor addressing:
 * backspace or DEL rubs out a character, Ctrl-U discards the line.  CR
 * and LF both end it.  The line is always NUL terminated, and the
 * terminator is not stored, so size must be at least 1.
 */
int kgets(char *buf, int size)
{
    int len = 0;
    int c;

    if (size < 1) {
        return 0;
    }

    for (;;) {
        c = kgetc();

        if (c == '\r' || c == '\n') {
            kputc('\n');
            break;
        }

        if (c == 0x08 || c == 0x7f) {           /* backspace or DEL */
            if (len > 0) {
                len--;
                kputs("\b \b");
            }
            continue;
        }

        if (c == 0x15) {                        /* Ctrl-U, kill line */
            while (len > 0) {
                len--;
                kputs("\b \b");
            }
            continue;
        }

        if (c < 32 || c > 126) {                /* ignore other control */
            continue;
        }

        if (len < size - 1) {
            buf[len++] = (char)c;
            kputc((char)c);
        }
        /* A full buffer silently refuses more rather than overrunning. */
    }

    buf[len] = '\0';
    return len;
}
