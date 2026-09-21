/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mfp.c - MC68901 Multi-Function Peripheral: interrupt controller and
 * the system timer.
 *
 * Reference: Motorola MC68901 datasheet; Motorola M68040 User's Manual
 * chapter 8 for the exception stack frame.
 *
 * The MFP is the board's interrupt controller. It drives one IPL line to
 * the 68040 and supplies its own vector during the acknowledge cycle, so
 * its sixteen channels arrive at sixteen consecutive vectors. Rather
 * than sixteen stubs, one stub is installed at all of them and works out
 * which channel it is from the vector the CPU dispatched through -- the
 * format/vector word the 68040 pushed is right there in the frame.
 *
 * It is also where the system tick comes from. Timer D at the /200
 * prescaler runs at 2457600/200 = 12288 Hz, and a reload of 123 divides
 * that to 99.9 Hz -- close enough to HZ that nothing can tell, and the
 * error is a known 0.1% rather than an unknown amount.
 *
 * THE LIVELOCK, which this machine has already walked into once. A timer
 * whose period is shorter than its own handler starves the foreground
 * completely: the handler returns, the next interrupt is already
 * pending, and no other instruction ever executes. Timer A at /4 with a
 * small reload is 13 us and does exactly that. 10 ms leaves four orders
 * of magnitude of headroom, which is the right kind of margin for
 * something with no way to complain.
 */
#include "dev.h"
#include "timer.h"
#include "errno.h"
#include "drivers.h"

#define MFP_VR_BASE   0x40      /* vectors 0x40..0x4f                 */

/* Timers C and D share one control register: D in bits 0-2, C in 4-6. */
#define TCDCR_D_MASK  0x07

/*
 * Per-channel handlers. Sixteen function pointers is the entire
 * interrupt controller as far as the rest of the kernel is concerned:
 * a driver asks for a channel and gets called on it.
 */
struct mfp_irq {
    void (*handler)(void *arg);
    void *arg;
};

static struct mfp_irq irqs[16];
static u32 spurious;

/* Channels 0-7 live in the B registers, 8-15 in the A registers. */
static void mfp_enable_channel(int ch)
{
    if (ch >= 8) {
        MMIO8(MFP_IERA) |= (u8)(1 << (ch - 8));
        MMIO8(MFP_IMRA) |= (u8)(1 << (ch - 8));
    } else {
        MMIO8(MFP_IERB) |= (u8)(1 << ch);
        MMIO8(MFP_IMRB) |= (u8)(1 << ch);
    }
}

static void mfp_disable_channel(int ch)
{
    if (ch >= 8) {
        MMIO8(MFP_IERA) &= (u8)~(1 << (ch - 8));
        MMIO8(MFP_IMRA) &= (u8)~(1 << (ch - 8));
    } else {
        MMIO8(MFP_IERB) &= (u8)~(1 << ch);
        MMIO8(MFP_IMRB) &= (u8)~(1 << ch);
    }
}

/* Writing a zero to a pending bit clears it; writing a one leaves it. */
static void mfp_clear_pending(int ch)
{
    if (ch >= 8) {
        MMIO8(MFP_IPRA) = (u8)~(1 << (ch - 8));
    } else {
        MMIO8(MFP_IPRB) = (u8)~(1 << ch);
    }
}

/*
 * Called from the stub with the vector the CPU dispatched through.
 *
 * The chip clears the channel's pending bit during the acknowledge
 * cycle, so there is nothing to acknowledge here. The vector register is
 * programmed for automatic end-of-interrupt -- software EOI exists to
 * let a handler hold off lower-priority channels, and nothing here wants
 * that.
 */
void mfp_dispatch(u32 vector)
{
    int ch = (int)(vector & 0x0f);

    if (irqs[ch].handler) {
        irqs[ch].handler(irqs[ch].arg);
    } else {
        /* Counted rather than ignored: an interrupt from a channel
         * nothing asked for means something is enabled that should not
         * be, and a silent one is very hard to find later. */
        spurious++;
    }
}

__asm__(
"       .text                               \n"
"       .globl _mfp_stub                    \n"
/*
 * EVERY register is saved, not just the scratch ones.
 *
 * The timer's handler can decide that the running task has had its turn,
 * and the switch that follows happens by swapping the kernel stack
 * pointer -- so whatever the interrupted task was holding has to be ON
 * that stack, or it is lost when another task's stack takes its place.
 * A stub that saved only d0-d1/a0-a1 was correct for a machine that
 * never switched and is not for one that does.
 */
"_mfp_stub:                                 \n"
"       movem.l %d0-%d7/%a0-%a6,-(%sp)      \n"   /* 60 bytes           */
"       moveq   #0,%d0                      \n"
"       move.w  66(%sp),%d0                 \n"   /* format/vector word */
"       andi.l  #0xfff,%d0                  \n"   /* vector offset      */
"       lsr.l   #2,%d0                      \n"   /* -> vector number   */
"       move.l  %d0,-(%sp)                  \n"
"       jsr     mfp_dispatch                \n"
"       addq.l  #4,%sp                      \n"
"       clr.l   -(%sp)                      \n"
"       move.w  64(%sp),2(%sp)              \n"   /* saved SR, low half */
"       jsr     task_ret_to_user            \n"
"       addq.l  #4,%sp                      \n"
"       movem.l (%sp)+,%d0-%d7/%a0-%a6      \n"
"       rte                                 \n"
);
extern void _mfp_stub(void);

int mfp_request_irq(int ch, void (*handler)(void *), void *arg)
{
    if (ch < 0 || ch > 15 || !handler) {
        return -EINVAL;
    }
    if (irqs[ch].handler) {
        return -EBUSY;
    }
    irqs[ch].handler = handler;
    irqs[ch].arg = arg;
    mfp_clear_pending(ch);
    mfp_enable_channel(ch);
    return 0;
}

u32 mfp_spurious(void)
{
    return spurious;
}

/* ---------------------------------------------------------------- */
/* The system timer                                                  */
/* ---------------------------------------------------------------- */

static void timer_d_isr(void *arg)
{
    (void)arg;
    timer_tick();
}

/*
 * The MFP's timer input is XTAL1 divided by the prescaler, and the timer
 * counts that down from its data register. So:
 *
 *      rate = 2457600 / prescaler / reload
 *
 * /200 gives 12288 Hz, which divides by 123 to 99.902 Hz. The reload is
 * computed rather than written down so that changing HZ does the right
 * thing, and it is clamped because a reload of 0 means 256 on this chip
 * and a reload of 1 would be 12 kHz -- straight into the livelock.
 */
static int mfp_timer_start(struct timerdev *t, u32 hz)
{
    u32 input = MFP_XTAL1 / 200;
    u32 reload;

    if (hz == 0) {
        return -EINVAL;
    }
    reload = (input + hz / 2) / hz;
    if (reload < 8) {
        return -EINVAL;         /* faster than the handler: refuse */
    }
    if (reload > 255) {
        reload = 255;
    }

    MMIO8(MFP_TDDR) = (u8)reload;
    MMIO8(MFP_TCDCR) = (u8)((MMIO8(MFP_TCDCR) & (u8)~TCDCR_D_MASK) |
                            MFP_TC_DIV200);

    t->hz = input / reload;

    return mfp_request_irq(MFPCH_TIMERD, timer_d_isr, 0);
}

static int mfp_timer_stop(struct timerdev *t)
{
    (void)t;
    MMIO8(MFP_TCDCR) &= (u8)~TCDCR_D_MASK;      /* prescaler 0 = stopped */
    mfp_disable_channel(MFPCH_TIMERD);
    return 0;
}

static struct timerdev mfp_timer = {
    "mfp-timer-d",
    0,
    mfp_timer_start,
    mfp_timer_stop,
    0
};

/* ---------------------------------------------------------------- */

/* Is the chip there? The vector register is writable and has no side
 * effects beyond the vectors it generates, and nothing is enabled yet. */
static int mfp_present(void)
{
    u8 saved = MMIO8(MFP_VR);
    int ok;

    MMIO8(MFP_VR) = 0x40;
    ok = ((MMIO8(MFP_VR) & 0xf8) == 0x40);
    MMIO8(MFP_VR) = saved;
    return ok;
}

int mfp_init(void)
{
    int i;

    /* Is the chip fitted? An address with nothing behind it raises a
     * bus error rather than reading back zeroes, so this has to be
     * asked before the first register access, not by making one. */
    if (!io_probe8((volatile void *)MFP_BASE)) {
        return -ENODEV;
    }

    if (!mfp_present()) {
        return -ENODEV;
    }

    /* Everything off before anything is pointed anywhere. */
    MMIO8(MFP_IERA) = 0;
    MMIO8(MFP_IERB) = 0;
    MMIO8(MFP_IMRA) = 0;
    MMIO8(MFP_IMRB) = 0;
    MMIO8(MFP_TACR) = 0;
    MMIO8(MFP_TBCR) = 0;
    MMIO8(MFP_TCDCR) = 0;

    for (i = 0; i < 16; i++) {
        irqs[i].handler = 0;
        irqs[i].arg = 0;
    }

    /* All sixteen vectors at the one stub. Only the top nibble of VR is
     * the base; the chip supplies the channel in the low nibble. */
    {
        volatile u32 *vectors = (volatile u32 *)_vectors;

        for (i = 0; i < 16; i++) {
            vectors[MFP_VR_BASE + i] = (u32)&_mfp_stub;
        }
    }
    MMIO8(MFP_VR) = MFP_VR_BASE;        /* automatic end-of-interrupt */

    return dev_register_timer(&mfp_timer);
}

/*
 * Let interrupts in.
 *
 * Deliberately separate from mfp_init(), and called last in startup: up
 * to this point a fault is reported by a handler that has the console to
 * itself, and an interrupt arriving in the middle of bringing a driver
 * up would be a much harder thing to understand.
 */
void mfp_interrupts_on(void)
{
    __asm__ volatile ("move.w #0x2000,%sr");
}
