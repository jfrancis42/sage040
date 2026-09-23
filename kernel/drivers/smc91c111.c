/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * smc91c111.c - the ethernet interface, an SMSC LAN91C111 in polled mode.
 *
 * Reference: SMSC LAN91C111 datasheet.  There are no descriptor rings.
 * Packets live in the chip's own 2 KiB pages, handed out by a packet
 * MMU, and the CPU reaches them through an auto-incrementing pointer
 * and a single data port.  A driver for this part is therefore mostly
 * bookkeeping: ask for a page, pour bytes into it, hand it back.
 *
 * What this driver does: brings the interface up and down, transmits
 * one frame at a time, and hands back one received frame at a time.
 *
 * What it does not do: interrupts.  The chip's IRQ reaches MFP channel
 * 3, but nothing is plumbed to service it, so the chip's interrupt mask
 * is deliberately left at zero and both paths poll the status register
 * instead.  Nor does it do multicast filtering, promiscuous mode, or
 * any of the statistics counters.
 *
 * ---------------------------------------------------------------------
 * The two traps.
 *
 * BYTE ORDER.  Registers and packet memory want opposite treatment, and
 * mixing them up is the classic way to lose a week here.
 *
 * A register is a 16-bit *value* whose low byte the part places at the
 * even offset.  That is exactly what a 16-bit access to this bus
 * produces, so MMIO16 on a register needs no swapping at all -- the
 * revision register reads 0x3391 and not 0x9133, which is the one-line
 * test for having got this right.
 *
 * Packet memory is not values, it is a byte stream, and its order is
 * fixed by ethernet rather than by the CPU.  So every access to the
 * data port in this file is MMIO8, one byte at a time, including the
 * two header words -- which is why the byte count below is written low
 * byte first even though the CPU is big-endian.  Byte-at-a-time is not
 * slow paranoia; it is the only access width whose meaning does not
 * depend on how the part was wired to the bus.
 *
 * ODD LENGTHS.  An odd-length frame is not simply padded.  The byte
 * count in the packet header covers the whole frame -- the status word,
 * the count itself, the data, and the trailing control byte -- and it
 * is always even.  An even-length frame therefore carries a filler byte
 * before the control byte and the count is len + 6; an odd-length frame
 * drops the filler, the count is len + 5, and ODD in the control byte
 * says the byte ahead of it is real data rather than filler.  Get that
 * backwards and every odd-length frame goes out a byte short or a byte
 * long, which surfaces as a protocol bug three layers up.
 * ---------------------------------------------------------------------
 */
#include "dev.h"
#include "drivers.h"
#include "errno.h"
#include "sage040.h"

/*
 * Bits sage040.h does not name, all from the datasheet's register
 * summary.  They live here rather than in the shared header because
 * nothing outside this file has any business with them.
 */
#define TCR_TXENA        0x0001  /* bank 0 TCR: transmitter enable     */
#define TCR_PAD_EN       0x0080  /* pad short frames out to 64 bytes   */
#define RCR_RXEN         0x0100  /* bank 0 RCR: receiver enable        */
#define RCR_STRIP        0x0200  /* drop the FCS before we see it      */

#define CTL_AUTO_RELEASE 0x0800  /* bank 1 CTL bit 11                  */
#define CTL_EEPROM       0x0003  /* STORE and RELOAD, see smc_up()     */

#define MMUCMD_BUSY      0x0001  /* bank 2 MMU command: command in use */

/*
 * MMU command 101: release the page named by the packet number
 * register.  sage040.h names commands 001, 010, 011, 100 and 110;
 * SMC_MMU_RELEASE there is command 100, "remove and release the frame
 * at the top of the receive FIFO", which is a different operation and
 * is the one the receive path wants.  This is the transmit-side one.
 */
#define MMU_RELEASE_PKT  0x00A0

#define SMC_B2_TXFIFO    SMC_REG(4)   /* transmit completion FIFO      */
#define SMC_B2_INTMASK   SMC_REG(13)  /* interrupt mask                */

#define FIFO_EMPTY       0x80    /* FIFO byte reads 0x80 when empty    */
#define FIFO_PNR         0x3F    /* the rest of it is a packet number  */
#define ALLOC_FAILED     0x80    /* same encoding in the alloc result  */

/* Receive status word, datasheet section on the RX frame format. */
#define RS_ALGNERR       0x8000
#define RS_BADCRC        0x2000
#define RS_ODDFRM        0x1000
#define RS_TOOLONG       0x0800
#define RS_TOOSHORT      0x0400
#define RS_ERRORS        (RS_ALGNERR | RS_BADCRC | RS_TOOLONG | RS_TOOSHORT)

#define TX_CTL_ODD       0x20    /* control byte: last data byte odd   */

/*
 * The byte count field is eleven bits; the rest of that word is
 * reserved and is not guaranteed to read back as zero.
 */
#define RX_COUNT_MASK    0x07FF
#define RX_OVERHEAD      6       /* status + count + filler + control  */

/* REV, bank 3: bits 7..4 are the chip ID, bits 3..0 the revision. */
#define REV_CHIPID       0x00F0
#define REV_LAN91C111    0x0090

#define ETH_HDR_LEN      14      /* nothing shorter is a frame         */
#define SMC_PAGES        4       /* pages of packet memory, MIR-sized  */

/*
 * Every wait in this file is bounded.  There is no scheduler to yield
 * to and no watchdog to catch us, so a driver that spins forever on a
 * chip that has stopped answering takes the whole machine with it; a
 * driver that gives up and reports -EIO leaves a machine you can still
 * type at.  The count is arbitrary -- it only has to be far longer than
 * any operation this chip actually performs.
 */
#define SMC_SPIN         100000

static void smc_bank(int b)
{
    MMIO16(SMC_BANKSEL) = (u16)b;
}

/*
 * Issue an MMU command.  The busy bit clears in a handful of cycles,
 * but it has to be checked: a command written while the previous one is
 * still running is simply lost, with no error anywhere.
 */
static int smc_mmu(u16 cmd)
{
    int spin;

    for (spin = 0; spin < SMC_SPIN; spin++) {
        if (!(MMIO16(SMC_B2_MMUCMD) & MMUCMD_BUSY)) {
            MMIO16(SMC_B2_MMUCMD) = cmd;
            return 0;
        }
    }
    return -EIO;
}

/*
 * Hand back the pages of frames the transmitter has finished with.
 *
 * AUTO RELEASE does this in hardware, so in the normal case the
 * completion FIFO is empty and this returns immediately.  It is here
 * because the chip has only four pages: if AUTO RELEASE is ever cleared
 * -- or turns out not to be honoured -- the fourth transmit is the last
 * one that ever works, and the symptom is "the network stopped", which
 * is a long way from the cause.  Caller must have bank 2 selected.
 */
static void smc_tx_reclaim(void)
{
    int i;
    u8 packet;

    for (i = 0; i < SMC_PAGES; i++) {
        packet = MMIO8(SMC_B2_TXFIFO);
        if (packet & FIFO_EMPTY) {
            return;
        }
        MMIO8(SMC_B2_PNR) = (u8)(packet & FIFO_PNR);
        if (smc_mmu(MMU_RELEASE_PKT) < 0) {
            return;
        }
        /* Acknowledging TX is what pops the completion FIFO. */
        MMIO8(SMC_B2_INT) = SMC_INT_TX;
    }
}

/*
 * Drop the frame at the top of the receive FIFO and give its page back.
 * Every exit from smc_recv() goes through here, including the error
 * ones: a frame left at the head of the FIFO is never followed by
 * another, so failing to release on the error path wedges the receiver
 * permanently rather than losing one packet.
 */
static void smc_rx_release(void)
{
    (void)smc_mmu(SMC_MMU_RELEASE);
}

static int smc_up(struct netdev *n)
{
    u16 ctl;

    (void)n;

    /*
     * Reset the packet MMU before anything else.  It owns the page
     * allocator, and pages allocated by whatever ran before us -- the
     * boot ROM, or an earlier life of this driver -- stay allocated
     * until it is told otherwise.
     */
    smc_bank(2);
    if (smc_mmu(SMC_MMU_RESET) < 0) {
        return -EIO;
    }

    /*
     * Mask every interrupt source at the chip.  The IRQ line reaches
     * MFP channel 3 and there is no handler for it; an unmasked source
     * would assert the line and leave it asserted.
     */
    MMIO8(SMC_B2_INTMASK) = 0x00;

    /*
     * AUTO RELEASE frees a transmitted frame's page without the driver
     * having to notice that the transmit finished -- which is what
     * makes a polled transmit path possible at all.
     *
     * Read-modify-write, and mask the low two bits out on the way past:
     * they are STORE and RELOAD, which start an EEPROM write and an
     * EEPROM read.  They read back as zero on a healthy part, but a
     * read-modify-write that trusts that is one flipped bit away from
     * rewriting the board's configuration EEPROM.
     */
    smc_bank(1);
    ctl = MMIO16(SMC_B1_CONTROL);
    MMIO16(SMC_B1_CONTROL) = (u16)((ctl | CTL_AUTO_RELEASE) & ~CTL_EEPROM);

    /*
     * PAD_EN makes the chip pad runt frames to the 64 bytes ethernet
     * requires, and STRIP makes it drop the FCS on receive, so nothing
     * above this file has to know either rule exists.
     */
    smc_bank(0);
    MMIO16(SMC_B0_TCR) = TCR_TXENA | TCR_PAD_EN;
    MMIO16(SMC_B0_RCR) = RCR_RXEN | RCR_STRIP;

    /* Read back: a chip that is not there accepts writes silently. */
    if (!(MMIO16(SMC_B0_TCR) & TCR_TXENA)) {
        return -EIO;
    }
    if (!(MMIO16(SMC_B0_RCR) & RCR_RXEN)) {
        return -EIO;
    }
    return 0;
}

static int smc_down(struct netdev *n)
{
    (void)n;

    /*
     * Receiver first.  Stopping the transmitter first would leave a
     * window in which frames still arrive and fill pages that nothing
     * is going to come and collect.
     */
    smc_bank(0);
    MMIO16(SMC_B0_RCR) = 0;
    MMIO16(SMC_B0_TCR) = 0;
    return 0;
}

static int smc_send(struct netdev *n, const void *frame, u32 len)
{
    const u8 *p = frame;
    u32 count;
    u32 i;
    int spin;
    int packet;

    (void)n;

    if (len < ETH_HDR_LEN || len > NET_MTU) {
        return -EINVAL;
    }

    smc_bank(2);
    smc_tx_reclaim();

    /*
     * TAKE A GRANT THAT IS ALREADY WAITING; DO NOT ASK TWICE.
     *
     * The allocation is a standing REQUEST, not a question. When no
     * page is free the chip remembers the request and satisfies it as
     * soon as one is released, raising ALLOC then. Issuing a second
     * allocate command clears ALLOC and starts a fresh request, which
     * abandons the page granted to the first -- and nothing ever
     * releases it. The chip has four, shared with the receiver, so
     * four abandoned grants leave it unable to send or receive at
     * all. See design.md, "The LAN91C111's four pages".
     *
     * So: ALLOC already up on the way in means the page named in the
     * result register is one we asked for and never collected.
     */
    packet = -1;
    if (MMIO8(SMC_B2_INT) & SMC_INT_ALLOC) {
        int granted = MMIO8(SMC_B2_PNR + 1);

        if (!(granted & ALLOC_FAILED)) {
            packet = granted;
        }
    }

    if (packet < 0) {
        /*
         * Ask for a page.  The allocate command clears ALLOC on its way in,
         * so the bit seen after it is this request's answer and not a stale
         * one from the previous frame.
         */
        if (smc_mmu(SMC_MMU_ALLOC_TX) < 0) {
            return -EIO;
        }
        for (spin = 0; spin < SMC_SPIN; spin++) {
            if (MMIO8(SMC_B2_INT) & SMC_INT_ALLOC) {
                break;
            }
        }
        if (!(MMIO8(SMC_B2_INT) & SMC_INT_ALLOC)) {
            /*
             * No page free. The request stands, the chip will grant
             * one when a page is released, and the next call through
             * here collects it above. Nothing is abandoned.
             */
            return -ENOMEM;
        }

        packet = MMIO8(SMC_B2_PNR + 1);
        if (packet & ALLOC_FAILED) {
                return -ENOMEM;
        }
    }

    MMIO8(SMC_B2_PNR) = (u8)packet;
    MMIO16(SMC_B2_PTR) = SMC_PTR_AUTOINC;   /* write, auto-increment */

    /*
     * The frame, in the layout the datasheet calls out: a status word
     * the chip fills in and we write as zero, the byte count for the
     * whole frame, the data, and the control byte.  See the header
     * comment for why an odd length costs five bytes of overhead and an
     * even length six.
     */
    count = (len & 1) ? len + 5 : len + 6;

    MMIO8(SMC_B2_DATA) = 0x00;
    MMIO8(SMC_B2_DATA) = 0x00;
    MMIO8(SMC_B2_DATA) = (u8)(count & 0xff);
    MMIO8(SMC_B2_DATA) = (u8)((count >> 8) & 0xff);

    for (i = 0; i < len; i++) {
        MMIO8(SMC_B2_DATA) = p[i];
    }

    if (len & 1) {
        MMIO8(SMC_B2_DATA) = TX_CTL_ODD;
    } else {
        MMIO8(SMC_B2_DATA) = 0x00;      /* filler                    */
        MMIO8(SMC_B2_DATA) = 0x00;      /* control byte, ODD clear   */
    }

    /*
     * Enqueue.  If the MMU will not take the command the page is lost
     * until the next smc_up(), which is the right trade: recovering it
     * means issuing another MMU command to a controller that has just
     * proved it is not accepting them.
     */
    if (smc_mmu(SMC_MMU_ENQUEUE) < 0) {
        return -EIO;
    }
    return 0;
}

static s32 smc_recv(struct netdev *n, void *frame, u32 max)
{
    u16 status;
    u32 count;
    u32 len;
    u32 i;
    u8 *p = frame;

    (void)n;

    smc_bank(2);

    /* Nothing waiting is the common case, and must not block. */
    if (!(MMIO8(SMC_B2_INT) & SMC_INT_RCV)) {
        return 0;
    }

    MMIO16(SMC_B2_PTR) = SMC_PTR_RCV | SMC_PTR_AUTOINC | SMC_PTR_READ;

    status  = MMIO8(SMC_B2_DATA);
    status |= (u16)((u16)MMIO8(SMC_B2_DATA) << 8);
    count   = MMIO8(SMC_B2_DATA);
    count  |= (u32)MMIO8(SMC_B2_DATA) << 8;
    count  &= RX_COUNT_MASK;

    if (count < RX_OVERHEAD) {
        smc_rx_release();
        return -EIO;
    }
    if (status & RS_ERRORS) {
        smc_rx_release();
        return -EIO;
    }

    /*
     * Back out the overhead the chip added.  ODDFRM says the frame had
     * an odd number of bytes, so the filler byte is absent and there is
     * one byte less to subtract.  The FCS is already gone -- RCR.STRIP
     * removed it -- so this is the frame length the caller wants.
     */
    len = count - ((status & RS_ODDFRM) ? 5u : 6u);

    if (len > max) {
        /*
         * Drop it rather than truncate.  A partially read frame would
         * leave the data pointer somewhere unhelpful, and half an
         * ethernet frame is not useful to anyone above.
         */
        smc_rx_release();
        return -E2BIG;
    }

    for (i = 0; i < len; i++) {
        p[i] = MMIO8(SMC_B2_DATA);
    }

    smc_rx_release();
    return (s32)len;
}

static struct netdev eth0 = {
    "eth0",
    { 0, 0, 0, 0, 0, 0 },
    smc_up,
    smc_down,
    smc_send,
    smc_recv,
    0,
    0
};

void smc91c111_init(void)
{
    u16 rev;
    int i;

    /* Is the chip fitted? An address with nothing behind it raises a
     * bus error rather than reading back zeroes, so this has to be
     * asked before the first register access, not by making one. */
    if (!io_probe16((volatile void *)SMC_BASE)) {
        return;
    }

    /*
     * Probe in two steps.  The bank select register is visible from
     * every bank and is the cheapest thing on the part that can be
     * shown to be a register rather than a floating bus -- write a bank
     * number, read it back.  Then confirm what the part is, because
     * something else answering at this address would also read back
     * whatever was written to it.
     */
    MMIO16(SMC_BANKSEL) = 3;
    if ((MMIO16(SMC_BANKSEL) & 0x07) != 3) {
        return;
    }

    rev = MMIO16(SMC_B3_REV);
    if ((rev & REV_CHIPID) != REV_LAN91C111) {
        return;
    }

    /*
     * The individual address registers hold the MAC the board was
     * configured with -- on this machine QEMU presets them -- so they
     * are read, never written.  Writing them here would mean inventing
     * an address the rest of the network has no reason to believe.
     */
    smc_bank(1);
    for (i = 0; i < NET_ADDR_LEN; i++) {
        eth0.mac[i] = MMIO8(SMC_B1_IA0 + i);
    }

    dev_register_net(&eth0);
}
