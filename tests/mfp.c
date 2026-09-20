/*
 * mfp.c - MC68901 interrupt plumbing shared by the MFP tests.
 *
 * Reference: Motorola MC68901 datasheet; Motorola M68040 User's Manual
 * for the exception stack frame.
 *
 * The MFP is a vectored interrupt source: it supplies its own vector
 * during the acknowledge cycle, so all sixteen of its channels arrive at
 * sixteen consecutive vectors.  Rather than write sixteen stubs, one stub
 * is installed at every one of them and recovers the vector number from
 * the exception frame the 68040 pushed.
 *
 * Frame layout for an interrupt (format 0, four words):
 *      SP+0   status register     (word)
 *      SP+2   program counter     (long)
 *      SP+6   format/vector word  (word)  bits 11-0 = vector * 4
 *
 * The stub pushes four longs before looking, so the format/vector word
 * sits at 16+6 = 22(%sp).
 */
#include "sage040.h"

volatile int mfp_irq_count;
volatile int mfp_last_vector;
volatile int mfp_last_channel;
volatile int mfp_vec_count[16];
volatile int mfp_isr_saw_inservice;   /* was ISR set when the handler ran? */
volatile int mfp_suppress_eoi;        /* leave the channel in service      */

/*
 * Called from the stub with the vector number the CPU dispatched through.
 *
 * The MFP clears the channel's pending bit during the acknowledge cycle,
 * and in software end-of-interrupt mode (VR bit 3 set) also flags the
 * channel in service.  An in-service channel inhibits itself and every
 * lower-priority channel until the handler signals end-of-interrupt by
 * clearing the bit, which is what mfp_eoi() does here.
 */
void mfp_isr_c(u32 vector)
{
    int ch = (int)(vector & 0x0f);

    mfp_last_vector = (int)vector;
    mfp_last_channel = ch;
    mfp_isr_saw_inservice = mfp_in_service(ch);
    mfp_irq_count++;
    mfp_vec_count[ch]++;

    /*
     * End of interrupt.  Harmless in automatic mode, where ISR is never
     * set.  A test can suppress it to hold the channel in service and
     * observe that lower-priority channels stay inhibited.
     */
    if (!mfp_suppress_eoi) {
        mfp_eoi(ch);
    }
}

__asm__(
"       .text                               \n"
"       .globl _mfp_isr                     \n"
"_mfp_isr:                                  \n"
"       movem.l %d0-%d1/%a0-%a1,-(%sp)      \n"
"       moveq   #0,%d0                      \n"
"       move.w  22(%sp),%d0                 \n"   /* format/vector word */
"       andi.l  #0xfff,%d0                  \n"   /* vector offset      */
"       lsr.l   #2,%d0                      \n"   /* -> vector number   */
"       move.l  %d0,-(%sp)                  \n"
"       jsr     mfp_isr_c                   \n"
"       addq.l  #4,%sp                      \n"
"       movem.l (%sp)+,%d0-%d1/%a0-%a1      \n"
"       rte                                 \n"
);
extern void _mfp_isr(void);

/* The vector table crt0.s laid down at address 0. */
extern volatile u32 _vectors[];

/*
 * Point all sixteen MFP vectors at the shared stub and program the
 * vector register so the chip generates them.  Only the top nibble of VR
 * is the base; the low nibble comes from the channel number.
 */
void mfp_install_vectors(u8 vr_base)
{
    int i;

    for (i = 0; i < 16; i++) {
        _vectors[(vr_base & 0xf0) + i] = (u32)&_mfp_isr;
    }
    MMIO8(MFP_VR) = (u8)(vr_base & 0xf0);
}

void mfp_reset_counts(void)
{
    int i;

    mfp_irq_count = 0;
    mfp_last_vector = -1;
    mfp_last_channel = -1;
    for (i = 0; i < 16; i++) {
        mfp_vec_count[i] = 0;
    }
}

/* Channels 0-7 live in the B registers, 8-15 in the A registers. */
void mfp_enable(int ch)
{
    if (ch >= 8) {
        MMIO8(MFP_IERA) |= (u8)(1 << (ch - 8));
        MMIO8(MFP_IMRA) |= (u8)(1 << (ch - 8));
    } else {
        MMIO8(MFP_IERB) |= (u8)(1 << ch);
        MMIO8(MFP_IMRB) |= (u8)(1 << ch);
    }
}

void mfp_disable(int ch)
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
void mfp_clear_pending(int ch)
{
    if (ch >= 8) {
        MMIO8(MFP_IPRA) = (u8)~(1 << (ch - 8));
    } else {
        MMIO8(MFP_IPRB) = (u8)~(1 << ch);
    }
}

int mfp_is_pending(int ch)
{
    if (ch >= 8) {
        return (MMIO8(MFP_IPRA) >> (ch - 8)) & 1;
    }
    return (MMIO8(MFP_IPRB) >> ch) & 1;
}

/* Is this channel flagged in service? */
int mfp_in_service(int ch)
{
    if (ch >= 8) {
        return (MMIO8(MFP_ISRA) >> (ch - 8)) & 1;
    }
    return (MMIO8(MFP_ISRB) >> ch) & 1;
}

/* Signal end-of-interrupt by clearing the in-service bit. */
void mfp_eoi(int ch)
{
    if (ch >= 8) {
        MMIO8(MFP_ISRA) = (u8)~(1 << (ch - 8));
    } else {
        MMIO8(MFP_ISRB) = (u8)~(1 << ch);
    }
}

u16 mfp_get_sr(void)
{
    u16 sr;
    __asm__ volatile ("move.w %%sr,%0" : "=d"(sr));
    return sr;
}

void mfp_set_ipl(int level)
{
    u16 sr = (u16)(0x2000 | ((level & 7) << 8));
    __asm__ volatile ("move.w %0,%%sr" :: "d"(sr) : "memory");
}
