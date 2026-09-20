/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * t6-irq.c - a peripheral interrupt travelling the whole chain.
 *
 * References: Motorola M68040 User's Manual (exception model),
 *             Motorola MC68901 datasheet (GPIP, vectors),
 *             TI/National PC16550D datasheet (IER / IIR).
 *
 * The other tests drive the MFP directly with its own timers.  This one
 * follows a real peripheral interrupt end to end:
 *
 *   NS16550A receive interrupt
 *     -> IRQ line into MFP GPIP5
 *       -> edge detected per AER, channel 7 becomes pending
 *         -> MFP asserts IPL 6 and supplies vector 0x47
 *           -> 68040 exception dispatch
 *             -> handler, then RTE
 *
 * The UART is put into local loopback so the byte it transmits arrives in
 * its own receiver, which is what raises the interrupt.
 */
#include "sage040.h"

#define IER_ERBFI       0x01        /* enable received-data interrupt    */
#define IIR_NO_INT      0x01        /* bit 0 set => no interrupt pending */
#define IIR_ID_MASK     0x0e
#define IIR_RX_DATA     0x04        /* received data available           */

#define VR_BASE         0x40
#define UART_CHANNEL    MFPCH_GPIP5 /* the UART is wired to GPIP5        */

int main(void)
{
    u8 v;
    int spin;

    test_begin("t6 peripheral interrupt through the MFP");

    mfp_set_ipl(7);
    MMIO8(MFP_TACR) = 0;
    MMIO8(MFP_TBCR) = 0;
    MMIO8(MFP_TCDCR) = 0;
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    MMIO8(MFP_IMRA) = 0;
    MMIO8(MFP_IMRB) = 0;

    uart_puts("  SR at entry = 0x"); uart_puthex16(mfp_get_sr()); uart_putc('\n');
    if (((mfp_get_sr() >> 8) & 7) == 7) {
        test_ok("interrupts start masked (IPL 7)");
    } else {
        test_fail("unexpected initial interrupt mask");
    }

    /* --- point the MFP's sixteen vectors at the shared stub --- */
    mfp_install_vectors(VR_BASE);
    MMIO8(MFP_VR) = VR_BASE;            /* automatic end-of-interrupt */
    if (MMIO8(MFP_VR) == VR_BASE) {
        test_ok("MFP vector base programmed to 0x40");
    } else {
        test_fail("could not program the vector register");
    }

    /* --- GPIP5 is an input, interrupting on the rising edge --- */
    MMIO8(MFP_DDR) &= (u8)~(1 << MFP_PIN_UART);
    MMIO8(MFP_AER) |= (u8)(1 << MFP_PIN_UART);
    mfp_enable(UART_CHANNEL);
    mfp_clear_pending(UART_CHANNEL);
    mfp_reset_counts();

    uart_puts("  GPIP at rest= 0x"); uart_puthex8(MMIO8(MFP_GPIP)); uart_putc('\n');
    if (!(MMIO8(MFP_GPIP) & (1 << MFP_PIN_UART))) {
        test_ok("UART interrupt line is low before it is armed");
    } else {
        test_fail("UART interrupt line is already asserted");
    }

    /* --- arm the UART: loopback, FIFOs off, receive interrupt on --- */
    MMIO8(UART_FCR) = 0x00;             /* one interrupt per byte */
    MMIO8(UART_MCR) = MCR_LOOP | MCR_DTR | MCR_RTS;
    MMIO8(UART_IER) = IER_ERBFI;

    if (MMIO8(UART_IIR) & IIR_NO_INT) {
        test_ok("UART reports no interrupt pending yet");
    } else {
        test_fail("UART already has an interrupt pending");
    }

    /* --- fire --- */
    mfp_set_ipl(0);
    MMIO8(UART_THR) = 0x5A;

    for (spin = 0; spin < 2000000 && mfp_irq_count == 0; spin++) {
        /* wait for the handler */
    }
    mfp_set_ipl(7);

    /* Read the character so the UART drops its interrupt line again. */
    v = 0;
    if (MMIO8(UART_LSR) & LSR_DR) {
        v = MMIO8(UART_RBR);
    }
    MMIO8(UART_IER) = 0x00;
    MMIO8(UART_MCR) = MCR_DTR | MCR_RTS;
    MMIO8(UART_FCR) = FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX;

    /* --- what happened --- */
    uart_puts("  irq_count   = "); uart_putdec((u32)mfp_irq_count); uart_putc('\n');
    uart_puts("  vector      = 0x"); uart_puthex8((u8)mfp_last_vector);
    uart_puts("  channel = "); uart_putdec((u32)mfp_last_channel); uart_putc('\n');
    uart_puts("  byte read   = 0x"); uart_puthex8(v); uart_putc('\n');

    if (mfp_irq_count > 0) {
        test_ok("INTERRUPT DELIVERED through the MFP");
    } else {
        test_fail("no interrupt was delivered");
        test_end();
        return 0;
    }

    if (mfp_last_channel == UART_CHANNEL) {
        test_ok("arrived on channel 7, the UART's GPIP5 pin");
    } else {
        test_fail("arrived on the wrong channel");
    }

    if (mfp_last_vector == (VR_BASE | UART_CHANNEL)) {
        test_ok("vector 0x47 = MFP base 0x40 ORed with channel 7");
    } else {
        test_fail("wrong vector");
    }

    if (v == 0x5A) {
        test_ok("the byte that caused the interrupt was recovered");
    } else {
        test_fail("wrong byte in the receiver");
    }

    if (!mfp_is_pending(UART_CHANNEL)) {
        test_ok("acknowledge cleared the channel's pending bit");
    } else {
        test_fail("pending bit survived the acknowledge");
    }

    if (((mfp_get_sr() >> 8) & 7) == 7) {
        test_ok("interrupts masked again cleanly");
    } else {
        test_fail("interrupt mask not restored");
    }

    test_ok("execution continued normally after the interrupt");

    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    test_end();
    return 0;
}
