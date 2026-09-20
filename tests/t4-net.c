/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * t4-net.c - SMSC LAN91C111 ethernet controller.
 *
 * Reference: SMSC LAN91C111 datasheet.
 *
 * The chip presents a 16-byte window whose meaning depends on the bank
 * selected through the bank-select register at offset 14.  Packets live
 * in on-chip memory reached through an auto-incrementing pointer and a
 * data port - there are no descriptor rings in host memory, which is
 * what makes this chip so much easier to drive than a SONIC or a LANCE.
 *
 * The final test transmits a real ARP request and waits for the reply
 * from QEMU's slirp gateway (10.0.2.2), so it exercises the whole path:
 * allocation, FIFO write, enqueue, receive interrupt and FIFO read.
 */
#include "sage040.h"

#define MY_IP0 10
#define MY_IP1 0
#define MY_IP2 2
#define MY_IP3 15

#define GW_IP0 10
#define GW_IP1 0
#define GW_IP2 2
#define GW_IP3 2

/* TCR / RCR bits from the datasheet */
#define TCR_TXENA   0x0001
#define TCR_PAD_EN  0x0080
#define RCR_RXEN    0x0100
#define RCR_STRIP   0x0200

static u8 mymac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

static void smc_bank(int b)
{
    MMIO16(SMC_BANKSEL) = (u16)b;
}

static void delay(int n)
{
    volatile int i;
    for (i = 0; i < n; i++) {
    }
}

int main(void)
{
    u16 rev, v;
    int i;

    test_begin("t4 SMSC LAN91C111 ethernet");

    /* --- bank select must be readable and stick --- */
    smc_bank(3);
    v = (u16)(MMIO16(SMC_BANKSEL) & 0x7);
    if (v == 3) {
        test_ok("bank select register works");
    } else {
        uart_puts("  banksel read back 0x"); uart_puthex16(v); uart_putc('\n');
        test_fail("bank select did not stick");
    }

    /* --- chip revision, bank 3 offset 10 --- */
    smc_bank(3);
    rev = MMIO16(SMC_B3_REV);
    uart_puts("  REV         = 0x"); uart_puthex16(rev); uart_putc('\n');
    if (rev == 0x3391) {
        test_ok("revision 0x3391 = LAN91C111 rev 1");
    } else if (rev == 0x9133) {
        test_fail("revision byte-swapped (0x9133) - endianness problem");
    } else {
        test_fail("unexpected revision - is the chip there?");
    }

    /* --- MAC address, bank 1 offsets 4..9 --- */
    smc_bank(1);
    for (i = 0; i < 6; i++) {
        MMIO8(SMC_B1_IA0 + i) = mymac[i];
    }
    uart_puts("  MAC         = ");
    {
        int good = 1;
        for (i = 0; i < 6; i++) {
            u8 b = MMIO8(SMC_B1_IA0 + i);
            uart_puthex8(b);
            if (i < 5) uart_putc(':');
            if (b != mymac[i]) good = 0;
        }
        uart_putc('\n');
        if (good) {
            test_ok("individual address registers read back correctly");
        } else {
            test_fail("individual address registers WRONG");
        }
    }

    /* --- reset the packet MMU, then enable TX and RX --- */
    smc_bank(2);
    MMIO16(SMC_B2_MMUCMD) = SMC_MMU_RESET;
    delay(1000);

    smc_bank(0);
    MMIO16(SMC_B0_TCR) = TCR_TXENA | TCR_PAD_EN;
    MMIO16(SMC_B0_RCR) = RCR_RXEN | RCR_STRIP;
    v = MMIO16(SMC_B0_TCR);
    uart_puts("  TCR         = 0x"); uart_puthex16(v); uart_putc('\n');
    if (v & TCR_TXENA) {
        test_ok("transmitter enabled (TCR.TXENA)");
    } else {
        test_fail("could not enable the transmitter");
    }
    v = MMIO16(SMC_B0_RCR);
    uart_puts("  RCR         = 0x"); uart_puthex16(v); uart_putc('\n');
    if (v & RCR_RXEN) {
        test_ok("receiver enabled (RCR.RXEN)");
    } else {
        test_fail("could not enable the receiver");
    }

    /* ------------------------------------------------------------ */
    /* Build and transmit an ARP request for the slirp gateway.      */
    /* ------------------------------------------------------------ */
    {
        u8 frame[42];
        int n = 0;
        int packetnum = -1;

        /* Ethernet header */
        for (i = 0; i < 6; i++) frame[n++] = 0xff;         /* broadcast */
        for (i = 0; i < 6; i++) frame[n++] = mymac[i];     /* source    */
        frame[n++] = 0x08; frame[n++] = 0x06;              /* ARP       */
        /* ARP payload */
        frame[n++] = 0x00; frame[n++] = 0x01;              /* ethernet  */
        frame[n++] = 0x08; frame[n++] = 0x00;              /* IPv4      */
        frame[n++] = 6;    frame[n++] = 4;
        frame[n++] = 0x00; frame[n++] = 0x01;              /* request   */
        for (i = 0; i < 6; i++) frame[n++] = mymac[i];     /* sender HW */
        frame[n++] = MY_IP0; frame[n++] = MY_IP1;
        frame[n++] = MY_IP2; frame[n++] = MY_IP3;
        for (i = 0; i < 6; i++) frame[n++] = 0x00;         /* target HW */
        frame[n++] = GW_IP0; frame[n++] = GW_IP1;
        frame[n++] = GW_IP2; frame[n++] = GW_IP3;

        /* 1. ask the packet MMU for a transmit buffer */
        smc_bank(2);
        MMIO16(SMC_B2_MMUCMD) = SMC_MMU_ALLOC_TX;

        for (i = 0; i < 100000; i++) {
            if (MMIO8(SMC_B2_INT) & SMC_INT_ALLOC) {
                break;
            }
        }
        if (MMIO8(SMC_B2_INT) & SMC_INT_ALLOC) {
            packetnum = MMIO8(SMC_B2_PNR + 1);   /* allocation result */
            uart_puts("  alloc packet= "); uart_putdec((u32)packetnum);
            uart_putc('\n');
            test_ok("packet MMU allocated a transmit buffer");
        } else {
            test_fail("packet MMU never signalled ALLOC");
            test_end();
            return 0;
        }

        /* 2. point at that packet and write status + length + data */
        MMIO8(SMC_B2_PNR) = (u8)packetnum;
        MMIO16(SMC_B2_PTR) = SMC_PTR_AUTOINC;   /* write, auto-increment */

        MMIO8(SMC_B2_DATA) = 0x00;              /* status word (ignored) */
        MMIO8(SMC_B2_DATA) = 0x00;
        MMIO8(SMC_B2_DATA) = (u8)((n + 6) & 0xff);   /* byte count lo */
        MMIO8(SMC_B2_DATA) = (u8)(((n + 6) >> 8) & 0xff);
        for (i = 0; i < n; i++) {
            MMIO8(SMC_B2_DATA) = frame[i];
        }
        MMIO8(SMC_B2_DATA) = 0x00;              /* pad (even length)     */
        MMIO8(SMC_B2_DATA) = 0x00;              /* control: ODD clear    */

        /* 3. hand it to the transmitter */
        MMIO16(SMC_B2_MMUCMD) = SMC_MMU_ENQUEUE;
        test_ok("ARP request enqueued for transmission");

        /* 4. wait for a received packet */
        for (i = 0; i < 2000000; i++) {
            if (MMIO8(SMC_B2_INT) & SMC_INT_RCV) {
                break;
            }
        }

        if (!(MMIO8(SMC_B2_INT) & SMC_INT_RCV)) {
            test_fail("no packet received (expected an ARP reply)");
            test_end();
            return 0;
        }
        test_ok("receive interrupt asserted - a packet arrived");

        /* 5. read it out of the FIFO */
        {
            u8 rx[64];
            int rxlen, j;

            MMIO16(SMC_B2_PTR) = SMC_PTR_RCV | SMC_PTR_AUTOINC | SMC_PTR_READ;
            (void)MMIO8(SMC_B2_DATA);           /* status lo */
            (void)MMIO8(SMC_B2_DATA);           /* status hi */
            rxlen  = MMIO8(SMC_B2_DATA);
            rxlen |= ((int)MMIO8(SMC_B2_DATA)) << 8;
            uart_puts("  rx length   = "); uart_putdec((u32)rxlen);
            uart_putc('\n');

            for (j = 0; j < 64 && j < rxlen; j++) {
                rx[j] = MMIO8(SMC_B2_DATA);
            }

            uart_puts("  rx ethertype= 0x");
            uart_puthex8(rx[12]); uart_puthex8(rx[13]); uart_putc('\n');
            uart_puts("  rx sender IP= ");
            uart_putdec(rx[28]); uart_putc('.');
            uart_putdec(rx[29]); uart_putc('.');
            uart_putdec(rx[30]); uart_putc('.');
            uart_putdec(rx[31]); uart_putc('\n');

            if (rx[12] == 0x08 && rx[13] == 0x06) {
                test_ok("received frame is ARP (ethertype 0x0806)");
            } else {
                test_fail("received frame is not ARP");
            }

            if (rx[20] == 0x00 && rx[21] == 0x02) {
                test_ok("it is an ARP REPLY (opcode 2)");
            } else {
                test_fail("not an ARP reply");
            }

            if (rx[28] == GW_IP0 && rx[29] == GW_IP1 &&
                rx[30] == GW_IP2 && rx[31] == GW_IP3) {
                test_ok("reply came from the gateway 10.0.2.2");
            } else {
                test_fail("reply came from an unexpected address");
            }

            /* release the receive buffer back to the MMU */
            MMIO16(SMC_B2_MMUCMD) = SMC_MMU_REMOVE_RX;
        }
    }

    test_end();
    return 0;
}
