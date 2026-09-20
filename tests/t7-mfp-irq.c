/*
 * t7-mfp-irq.c - MC68901 MFP interrupt controller.
 *
 * Reference: Motorola MC68901 Multi-Function Peripheral datasheet.
 *
 * The MFP is the system interrupt controller on this board, so this is
 * the test that matters most for writing an OS on it.  Covered here:
 *
 *   - the register file
 *   - IER gating: a disabled channel does not even become pending
 *   - IPR/ISR write-a-zero-to-clear semantics
 *   - IMR masking of an otherwise deliverable interrupt
 *   - vector generation from the vector register
 *   - priority: the highest-numbered ready channel wins
 *   - software end-of-interrupt and in-service inhibition
 *
 * Timer D is used as a convenient interrupt source throughout; the
 * timers themselves are tested in t8.
 */
#include "sage040.h"

/*
 * Timer settings used throughout.  /200 with a reload of 50 gives a
 * period of 10000 timer-clock cycles, about 4 ms at 2.4576 MHz.
 *
 * This is deliberately slow.  An earlier version of this test used /4
 * with a reload of 8 - a 13 us period - and livelocked the machine: the
 * interrupt handler takes longer to run than the interval between
 * interrupts, so the interrupted code never makes progress.  That is a
 * real property of the hardware, not an emulation artefact, and it is
 * worth remembering when picking a scheduler tick.
 */
#define T_PRESCALE  MFP_TC_DIV200
#define T_RELOAD    50

static void timer_d_start(u8 prescale, u8 reload)
{
    MMIO8(MFP_TDDR) = reload;
    MMIO8(MFP_TCDCR) = (u8)((MMIO8(MFP_TCDCR) & 0x70) | (prescale & 7));
}

static void timer_d_stop(void)
{
    MMIO8(MFP_TCDCR) = (u8)(MMIO8(MFP_TCDCR) & 0x70);
}

static void timer_c_start(u8 prescale, u8 reload)
{
    MMIO8(MFP_TCDR) = reload;
    MMIO8(MFP_TCDCR) = (u8)((MMIO8(MFP_TCDCR) & 0x07) | ((prescale & 7) << 4));
}

static void timer_c_stop(void)
{
    MMIO8(MFP_TCDCR) = (u8)(MMIO8(MFP_TCDCR) & 0x07);
}

/*
 * Arm a channel so that exactly one interrupt is waiting: run the timer
 * until the channel is pending, then stop it.  Leaving the timer running
 * while interrupts are unmasked risks the livelock described above.
 */
static int wait_pending(int ch, int spins);

static int arm_timer_d(void)
{
    mfp_clear_pending(MFPCH_TIMERD);
    timer_d_start(T_PRESCALE, T_RELOAD);
    if (!wait_pending(MFPCH_TIMERD, 20000000)) {
        timer_d_stop();
        return 0;
    }
    timer_d_stop();
    return 1;
}

static int arm_timer_c(void)
{
    mfp_clear_pending(MFPCH_TIMERC);
    timer_c_start(T_PRESCALE, T_RELOAD);
    if (!wait_pending(MFPCH_TIMERC, 20000000)) {
        timer_c_stop();
        return 0;
    }
    timer_c_stop();
    return 1;
}

/* Spin until a channel is pending, or give up. */
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

static int wait_irq(int spins)
{
    int i;

    for (i = 0; i < spins && mfp_irq_count == 0; i++) {
        /* wait for the handler to run */
    }
    return mfp_irq_count;
}

int main(void)
{
    u8 v;

    test_begin("t7 MC68901 interrupt controller");

    mfp_set_ipl(7);                 /* interrupts masked while we set up */
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    MMIO8(MFP_IMRA) = 0;
    MMIO8(MFP_IMRB) = 0;
    timer_c_stop();
    timer_d_stop();

    /* ---------------------------------------------------------- */
    /* 1. Register file                                            */
    /* ---------------------------------------------------------- */
    MMIO8(MFP_AER) = 0xA5;
    MMIO8(MFP_DDR) = 0x3C;
    MMIO8(MFP_SCR) = 0x5A;
    MMIO8(MFP_UCR) = 0x88;
    if (MMIO8(MFP_AER) == 0xA5 && MMIO8(MFP_DDR) == 0x3C &&
        MMIO8(MFP_SCR) == 0x5A && MMIO8(MFP_UCR) == 0x88) {
        test_ok("AER / DDR / SCR / UCR hold their values");
    } else {
        test_fail("register file did not read back");
    }

    MMIO8(MFP_IERA) = 0x0F;
    MMIO8(MFP_IMRA) = 0xF0;
    if (MMIO8(MFP_IERA) == 0x0F && MMIO8(MFP_IMRA) == 0xF0) {
        test_ok("IERA / IMRA hold their values");
    } else {
        test_fail("IERA / IMRA WRONG");
    }
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IMRA) = 0;

    MMIO8(MFP_VR) = 0x48;
    v = MMIO8(MFP_VR);
    uart_puts("  VR          = 0x"); uart_puthex8(v); uart_putc('\n');
    if (v == 0x48) {
        test_ok("vector register holds base 0x40 with the S bit set");
    } else {
        test_fail("vector register WRONG");
    }

    /* ---------------------------------------------------------- */
    /* 2. A disabled channel never becomes pending                 */
    /* ---------------------------------------------------------- */
    mfp_install_vectors(0x40);
    MMIO8(MFP_VR) = 0x40;           /* automatic end-of-interrupt */
    mfp_disable(MFPCH_TIMERD);
    mfp_clear_pending(MFPCH_TIMERD);

    timer_d_start(T_PRESCALE, T_RELOAD);
    if (!wait_pending(MFPCH_TIMERD, 20000000)) {
        test_ok("disabled channel does not set its pending bit");
    } else {
        test_fail("disabled channel became pending");
    }
    timer_d_stop();

    /* ---------------------------------------------------------- */
    /* 3. An enabled channel does                                  */
    /* ---------------------------------------------------------- */
    mfp_enable(MFPCH_TIMERD);
    mfp_clear_pending(MFPCH_TIMERD);
    timer_d_start(T_PRESCALE, T_RELOAD);
    if (wait_pending(MFPCH_TIMERD, 20000000)) {
        test_ok("enabled channel sets its pending bit");
    } else {
        test_fail("enabled channel never became pending");
        test_end();
        return 0;
    }
    timer_d_stop();

    /* ---------------------------------------------------------- */
    /* 4. IPR: writing ones leaves it, writing zeros clears it     */
    /* ---------------------------------------------------------- */
    MMIO8(MFP_IPRB) = 0xFF;
    if (mfp_is_pending(MFPCH_TIMERD)) {
        test_ok("writing ones to IPR leaves the pending bit set");
    } else {
        test_fail("writing ones to IPR cleared the bit");
    }

    MMIO8(MFP_IPRB) = (u8)~(1 << MFPCH_TIMERD);
    if (!mfp_is_pending(MFPCH_TIMERD)) {
        test_ok("writing a zero to IPR clears the pending bit");
    } else {
        test_fail("writing a zero to IPR did not clear the bit");
    }

    /* ---------------------------------------------------------- */
    /* 5. IMR masks an otherwise deliverable interrupt             */
    /* ---------------------------------------------------------- */
    mfp_reset_counts();
    MMIO8(MFP_IERB) |= (1 << MFPCH_TIMERD);     /* enabled ... */
    MMIO8(MFP_IMRB) &= (u8)~(1 << MFPCH_TIMERD); /* ... but masked */
    arm_timer_d();
    mfp_set_ipl(0);
    wait_irq(500000);
    mfp_set_ipl(7);

    if (mfp_irq_count == 0 && mfp_is_pending(MFPCH_TIMERD)) {
        test_ok("masked channel stays pending and is not delivered");
    } else {
        uart_puts("  irq_count="); uart_putdec((u32)mfp_irq_count);
        uart_puts(" pending="); uart_putdec((u32)mfp_is_pending(MFPCH_TIMERD));
        uart_putc('\n');
        test_fail("masked channel behaved incorrectly");
    }

    /* Unmasking it should let the already-pending interrupt through. */
    mfp_reset_counts();
    MMIO8(MFP_IMRB) |= (1 << MFPCH_TIMERD);
    mfp_set_ipl(0);
    wait_irq(500000);
    mfp_set_ipl(7);

    if (mfp_irq_count > 0) {
        test_ok("unmasking delivers the interrupt that was held");
    } else {
        test_fail("unmasking did not deliver the interrupt");
    }

    /* ---------------------------------------------------------- */
    /* 6. Vector generation                                        */
    /* ---------------------------------------------------------- */
    uart_puts("  last vector = 0x"); uart_puthex8((u8)mfp_last_vector);
    uart_puts("  channel = "); uart_putdec((u32)mfp_last_channel);
    uart_putc('\n');
    if (mfp_last_vector == (0x40 | MFPCH_TIMERD)) {
        test_ok("vector is VR base 0x40 ORed with the channel number");
    } else {
        test_fail("vector generation WRONG");
    }

    /* Move the base and check again. */
    mfp_install_vectors(0x60);
    MMIO8(MFP_VR) = 0x60;
    mfp_reset_counts();
    arm_timer_d();
    mfp_set_ipl(0);
    wait_irq(500000);
    mfp_set_ipl(7);

    uart_puts("  last vector = 0x"); uart_puthex8((u8)mfp_last_vector);
    uart_putc('\n');
    if (mfp_irq_count > 0 && mfp_last_vector == (0x60 | MFPCH_TIMERD)) {
        test_ok("moving the vector base moves the delivered vector");
    } else {
        test_fail("vector base change WRONG");
    }

    /* ---------------------------------------------------------- */
    /* 7. Priority: the higher-numbered channel wins               */
    /* ---------------------------------------------------------- */
    mfp_install_vectors(0x40);
    MMIO8(MFP_VR) = 0x40;
    mfp_enable(MFPCH_TIMERC);
    mfp_enable(MFPCH_TIMERD);
    mfp_clear_pending(MFPCH_TIMERC);
    mfp_clear_pending(MFPCH_TIMERD);
    mfp_reset_counts();

    /* Both become pending while the CPU has interrupts masked. */
    arm_timer_c();
    arm_timer_d();

    if (mfp_is_pending(MFPCH_TIMERC) && mfp_is_pending(MFPCH_TIMERD)) {
        test_ok("timer C and timer D are both pending simultaneously");

        mfp_set_ipl(0);
        wait_irq(500000);
        mfp_set_ipl(7);

        uart_puts("  first delivered channel = ");
        uart_putdec((u32)mfp_vec_count[MFPCH_TIMERC] ? MFPCH_TIMERC
                                                     : MFPCH_TIMERD);
        uart_putc('\n');
        if (mfp_vec_count[MFPCH_TIMERC] > 0) {
            test_ok("timer C (channel 5) was serviced before timer D (4)");
        } else {
            test_fail("priority order WRONG - lower channel won");
        }
    } else {
        test_fail("could not get both timers pending at once");
    }

    mfp_set_ipl(7);
    timer_c_stop();
    timer_d_stop();
    mfp_clear_pending(MFPCH_TIMERC);
    mfp_clear_pending(MFPCH_TIMERD);

    /* ---------------------------------------------------------- */
    /* 8. Software end-of-interrupt: the channel goes in service   */
    /* ---------------------------------------------------------- */
    MMIO8(MFP_VR) = 0x40 | MFP_VR_S;        /* software EOI mode */
    MMIO8(MFP_ISRA) = 0;
    MMIO8(MFP_ISRB) = 0;
    mfp_reset_counts();
    mfp_suppress_eoi = 1;                   /* hold it in service */

    arm_timer_c();
    mfp_set_ipl(0);
    wait_irq(500000);
    mfp_set_ipl(7);

    if (mfp_irq_count > 0 && mfp_isr_saw_inservice) {
        test_ok("in software EOI mode the channel is in service in the handler");
    } else {
        uart_puts("  irq_count="); uart_putdec((u32)mfp_irq_count);
        uart_puts(" saw_inservice="); uart_putdec((u32)mfp_isr_saw_inservice);
        uart_putc('\n');
        test_fail("in-service bit was not set at acknowledge");
    }

    if (mfp_in_service(MFPCH_TIMERC)) {
        test_ok("in-service bit persists after the handler returns");
    } else {
        test_fail("in-service bit did not persist");
    }

    /* ---------------------------------------------------------- */
    /* 9. In-service inhibits lower priority                       */
    /* ---------------------------------------------------------- */
    mfp_reset_counts();
    mfp_suppress_eoi = 0;
    arm_timer_d();

    mfp_set_ipl(0);
    wait_irq(500000);
    mfp_set_ipl(7);

    if (mfp_irq_count == 0 && mfp_is_pending(MFPCH_TIMERD)) {
        test_ok("timer C in service inhibits the lower-priority timer D");
    } else {
        uart_puts("  irq_count="); uart_putdec((u32)mfp_irq_count); uart_putc('\n');
        test_fail("in-service did not inhibit a lower-priority channel");
    }

    /* Signalling end-of-interrupt releases it. */
    mfp_eoi(MFPCH_TIMERC);
    mfp_set_ipl(0);
    wait_irq(500000);
    mfp_set_ipl(7);

    if (mfp_irq_count > 0 && mfp_last_channel == MFPCH_TIMERD) {
        test_ok("end-of-interrupt releases the inhibited channel");
    } else {
        test_fail("channel stayed inhibited after end-of-interrupt");
    }

    /* Tidy up so later output is not interrupted. */
    mfp_set_ipl(7);
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    timer_c_stop();
    timer_d_stop();

    test_end();
    return 0;
}
