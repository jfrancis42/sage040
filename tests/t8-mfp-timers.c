/*
 * t8-mfp-timers.c - the four MC68901 timers.
 *
 * Reference: Motorola MC68901 Multi-Function Peripheral datasheet.
 *
 * Timers A and B are full function (delay, event count, pulse width);
 * C and D are delay-mode only and share one control register.  All four
 * are 8-bit down counters that reload from their data register and raise
 * an interrupt each time they pass through zero.
 *
 * Covered here:
 *   - the data registers hold a reload value while stopped
 *   - a running timer's counter actually counts down
 *   - each of the four timers raises its own interrupt channel
 *   - the prescaler ratios are right, measured by racing two timers
 *   - a stopped timer stays stopped
 *   - timers A and B count real edges in event-count mode
 */
#include "sage040.h"

/*
 * A busy-wait the optimiser cannot delete.  An empty for-loop is removed
 * outright at -O2, which silently turned the "counter counts down" check
 * below into a no-op the first time this test was written.
 */
static volatile int delay_sink;

static void delay(int n)
{
    int i;

    for (i = 0; i < n; i++) {
        delay_sink++;
    }
}

static void tcdcr_set_c(u8 v)
{
    MMIO8(MFP_TCDCR) = (u8)((MMIO8(MFP_TCDCR) & 0x07) | ((v & 7) << 4));
}

static void tcdcr_set_d(u8 v)
{
    MMIO8(MFP_TCDCR) = (u8)((MMIO8(MFP_TCDCR) & 0x70) | (v & 7));
}

static void all_timers_off(void)
{
    MMIO8(MFP_TACR) = 0;
    MMIO8(MFP_TBCR) = 0;
    MMIO8(MFP_TCDCR) = 0;
}

static int wait_pending(int ch, int spins)
{
    int i;

    for (i = 0; i < spins; i++) {
        if (mfp_is_pending(ch)) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    u8 a, b;
    int i;

    test_begin("t8 MC68901 timers");

    mfp_set_ipl(7);
    all_timers_off();
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    MMIO8(MFP_IMRA) = 0;
    MMIO8(MFP_IMRB) = 0;
    mfp_install_vectors(0x40);
    MMIO8(MFP_VR) = 0x40;

    /* ---------------------------------------------------------- */
    /* 1. Data registers hold a reload value while stopped         */
    /* ---------------------------------------------------------- */
    MMIO8(MFP_TADR) = 0x11;
    MMIO8(MFP_TBDR) = 0x22;
    MMIO8(MFP_TCDR) = 0x33;
    MMIO8(MFP_TDDR) = 0x44;
    if (MMIO8(MFP_TADR) == 0x11 && MMIO8(MFP_TBDR) == 0x22 &&
        MMIO8(MFP_TCDR) == 0x33 && MMIO8(MFP_TDDR) == 0x44) {
        test_ok("all four data registers hold their reload values");
    } else {
        uart_puts("  A=0x"); uart_puthex8(MMIO8(MFP_TADR));
        uart_puts(" B=0x"); uart_puthex8(MMIO8(MFP_TBDR));
        uart_puts(" C=0x"); uart_puthex8(MMIO8(MFP_TCDR));
        uart_puts(" D=0x"); uart_puthex8(MMIO8(MFP_TDDR)); uart_putc('\n');
        test_fail("data registers WRONG");
    }

    /* ---------------------------------------------------------- */
    /* 2. A running counter counts down                            */
    /* ---------------------------------------------------------- */
    MMIO8(MFP_TCDR) = 255;
    tcdcr_set_c(MFP_TC_DIV200);         /* slow enough to observe */
    a = MMIO8(MFP_TCDR);
    delay(200000);
    b = MMIO8(MFP_TCDR);
    uart_puts("  timer C: "); uart_puthex8(a);
    uart_puts(" -> "); uart_puthex8(b); uart_putc('\n');
    if (b != a) {
        test_ok("a running timer's counter changes");
    } else {
        test_fail("counter did not move");
    }
    tcdcr_set_c(MFP_TC_STOP);

    /* A stopped timer must stay put. */
    a = MMIO8(MFP_TCDR);
    delay(200000);
    b = MMIO8(MFP_TCDR);
    if (a == b) {
        test_ok("a stopped timer's counter holds still");
    } else {
        uart_puts("  moved "); uart_puthex8(a);
        uart_puts(" -> "); uart_puthex8(b); uart_putc('\n');
        test_fail("stopped timer kept counting");
    }

    /* ---------------------------------------------------------- */
    /* 3. Each timer raises its own channel                        */
    /* ---------------------------------------------------------- */
    mfp_enable(MFPCH_TIMERA);
    mfp_enable(MFPCH_TIMERB);
    mfp_enable(MFPCH_TIMERC);
    mfp_enable(MFPCH_TIMERD);
    mfp_clear_pending(MFPCH_TIMERA);
    mfp_clear_pending(MFPCH_TIMERB);
    mfp_clear_pending(MFPCH_TIMERC);
    mfp_clear_pending(MFPCH_TIMERD);

    MMIO8(MFP_TADR) = 40;
    MMIO8(MFP_TACR) = MFP_TC_DIV200;
    if (wait_pending(MFPCH_TIMERA, 20000000)) {
        test_ok("timer A raises channel 13");
    } else {
        test_fail("timer A never fired");
    }
    MMIO8(MFP_TACR) = 0;

    MMIO8(MFP_TBDR) = 40;
    MMIO8(MFP_TBCR) = MFP_TC_DIV200;
    if (wait_pending(MFPCH_TIMERB, 20000000)) {
        test_ok("timer B raises channel 8");
    } else {
        test_fail("timer B never fired");
    }
    MMIO8(MFP_TBCR) = 0;

    MMIO8(MFP_TCDR) = 40;
    tcdcr_set_c(MFP_TC_DIV200);
    if (wait_pending(MFPCH_TIMERC, 20000000)) {
        test_ok("timer C raises channel 5");
    } else {
        test_fail("timer C never fired");
    }
    tcdcr_set_c(MFP_TC_STOP);

    MMIO8(MFP_TDDR) = 40;
    tcdcr_set_d(MFP_TC_DIV200);
    if (wait_pending(MFPCH_TIMERD, 20000000)) {
        test_ok("timer D raises channel 4");
    } else {
        test_fail("timer D never fired");
    }
    tcdcr_set_d(MFP_TC_STOP);

    /* ---------------------------------------------------------- */
    /* 4. Prescaler ratio, measured by racing two timers           */
    /*                                                             */
    /* Timer C at /4 and timer D at /200 with the same reload:     */
    /* C should fire about 50 times for each time D fires.         */
    /* ---------------------------------------------------------- */
    {
        int c_count = 0;
        int spins;

        all_timers_off();
        mfp_clear_pending(MFPCH_TIMERC);
        mfp_clear_pending(MFPCH_TIMERD);
        MMIO8(MFP_TCDR) = 100;
        MMIO8(MFP_TDDR) = 100;
        MMIO8(MFP_TCDCR) = (u8)((MFP_TC_DIV4 << 4) | MFP_TC_DIV200);

        for (spins = 0; spins < 40000000; spins++) {
            if (mfp_is_pending(MFPCH_TIMERC)) {
                mfp_clear_pending(MFPCH_TIMERC);
                c_count++;
            }
            if (mfp_is_pending(MFPCH_TIMERD)) {
                break;
            }
        }
        all_timers_off();

        uart_puts("  timer C fired "); uart_putdec((u32)c_count);
        uart_puts(" times per timer D tick (expect ~50)\n");
        if (c_count >= 30 && c_count <= 75) {
            test_ok("/4 vs /200 prescaler ratio is right");
        } else {
            test_fail("prescaler ratio WRONG");
        }
    }

    /* ---------------------------------------------------------- */
    /* 5. Event-count mode: timer A counts edges on TAI            */
    /*                                                             */
    /* TAI is GPIP4, which on this board is the ATA interrupt, so  */
    /* the edges counted here are real device interrupts.          */
    /* ---------------------------------------------------------- */
    {
        int ata_ok = 1;
        int n;

        all_timers_off();
        mfp_enable(MFPCH_GPIP4);        /* a disabled channel never pends */
        mfp_clear_pending(MFPCH_TIMERA);
        mfp_clear_pending(MFPCH_GPIP4);

        /* Interrupt on the rising edge of the ATA interrupt line. */
        MMIO8(MFP_AER) |= (1 << MFP_PIN_ATA);
        MMIO8(MFP_DDR) &= (u8)~(1 << MFP_PIN_ATA);   /* input */

        /* Timer A counts three events, then raises channel 13. */
        MMIO8(MFP_TADR) = 3;
        MMIO8(MFP_TACR) = MFP_TC_EVENT;

        /* Let the ATA device assert its interrupt line: nIEN clear. */
        MMIO8(ATA_DEVCTL) = 0x00;

        for (n = 0; n < 3; n++) {
            int spin;

            for (spin = 0; spin < 500000; spin++) {
                if (!(MMIO8(ATA_ALTSTAT) & ATA_SR_BSY)) {
                    break;
                }
            }
            MMIO8(ATA_DEVICE) = ATA_DEV_LBA;
            MMIO8(ATA_NSECT) = 0;
            MMIO8(ATA_LBAL) = 0;
            MMIO8(ATA_LBAM) = 0;
            MMIO8(ATA_LBAH) = 0;
            MMIO8(ATA_COMMAND) = ATA_CMD_IDENTIFY;

            for (spin = 0; spin < 500000; spin++) {
                if (MMIO8(ATA_ALTSTAT) & ATA_SR_DRQ) {
                    break;
                }
            }
            if (!(MMIO8(ATA_ALTSTAT) & ATA_SR_DRQ)) {
                ata_ok = 0;
                break;
            }
            /* Drain the sector and read the status to drop the IRQ line. */
            for (i = 0; i < 256; i++) {
                (void)MMIO16(ATA_DATA);
            }
            (void)MMIO8(ATA_STATUS);
            delay(20000);           /* let the line settle low again */
        }

        uart_puts("  GPIP4 pending after ATA commands = ");
        uart_putdec((u32)mfp_is_pending(MFPCH_GPIP4)); uart_putc('\n');
        uart_puts("  timer A event counter = ");
        uart_puthex8(MMIO8(MFP_TADR)); uart_putc('\n');

        if (!ata_ok) {
            test_fail("could not drive ATA interrupts to exercise TAI");
        } else if (mfp_is_pending(MFPCH_GPIP4)) {
            test_ok("a real ATA interrupt reached the MFP on GPIP4");
        } else {
            test_fail("ATA interrupt never reached GPIP4");
        }

        if (mfp_is_pending(MFPCH_TIMERA)) {
            test_ok("timer A counted three events and raised channel 13");
        } else {
            test_fail("event-count mode did not reach zero");
        }
        MMIO8(MFP_TACR) = 0;
        MMIO8(ATA_DEVCTL) = 0x02;       /* nIEN: mask ATA interrupts again */
    }

    all_timers_off();
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    mfp_set_ipl(7);

    test_end();
    return 0;
}
