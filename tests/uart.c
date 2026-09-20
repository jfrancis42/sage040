/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * uart.c - NS16550A console driver for the Sage040.
 *
 * Reference: TI/National PC16550D datasheet.  The chip is at 0xff000000
 * with byte-spaced registers, so register N of the datasheet is simply
 * byte N of the window.
 */
#include "sage040.h"

static int failures;
static int checks;

void uart_init(void)
{
    /* 8N1, no interrupts, FIFOs on, divisor 1 (baud is a no-op under
     * emulation but we program it anyway so the sequence is realistic). */
    MMIO8(UART_IER) = 0x00;
    MMIO8(UART_LCR) = LCR_DLAB;
    MMIO8(UART_DLL) = 0x01;
    MMIO8(UART_DLM) = 0x00;
    MMIO8(UART_LCR) = LCR_8N1;
    MMIO8(UART_FCR) = FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX;
    MMIO8(UART_MCR) = MCR_DTR | MCR_RTS;
}

void uart_putc(char c)
{
    while (!(MMIO8(UART_LSR) & LSR_THRE)) {
        /* spin until the transmit holding register is empty */
    }
    MMIO8(UART_THR) = (u8)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

int uart_rx_ready(void)
{
    return (MMIO8(UART_LSR) & LSR_DR) != 0;
}

u8 uart_getc(void)
{
    while (!uart_rx_ready()) {
        /* spin until a character arrives */
    }
    return MMIO8(UART_RBR);
}

static const char hexdigits[] = "0123456789ABCDEF";

void uart_puthex8(u8 v)
{
    uart_putc(hexdigits[(v >> 4) & 0xf]);
    uart_putc(hexdigits[v & 0xf]);
}

void uart_puthex16(u16 v)
{
    uart_puthex8((u8)(v >> 8));
    uart_puthex8((u8)v);
}

void uart_puthex32(u32 v)
{
    uart_puthex16((u16)(v >> 16));
    uart_puthex16((u16)v);
}

void uart_putdec(u32 v)
{
    char buf[12];
    int i = 0;

    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v > 0) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

void test_begin(const char *name)
{
    uart_init();
    failures = 0;
    checks = 0;
    uart_puts("\n=== ");
    uart_puts(name);
    uart_puts(" ===\n");
}

void test_ok(const char *what)
{
    checks++;
    uart_puts("  [ OK ] ");
    uart_puts(what);
    uart_putc('\n');
}

void test_fail(const char *what)
{
    checks++;
    failures++;
    uart_puts("  [FAIL] ");
    uart_puts(what);
    uart_putc('\n');
}

int test_failures(void)
{
    return failures;
}

void test_end(void)
{
    uart_puts("--- ");
    uart_putdec((u32)(checks - failures));
    uart_putc('/');
    uart_putdec((u32)checks);
    uart_puts(" checks passed");
    if (failures) {
        uart_puts(", ");
        uart_putdec((u32)failures);
        uart_puts(" FAILED");
    }
    uart_puts(" ---\n");
    /* Sentinel the harness greps for. */
    uart_puts(failures ? "RESULT: FAIL\n" : "RESULT: PASS\n");
}
