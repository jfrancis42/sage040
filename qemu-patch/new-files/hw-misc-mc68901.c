/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Motorola MC68901 Multi-Function Peripheral (MFP)
 *
 * Reference: Motorola MC68901 Multi-Function Peripheral datasheet.
 *
 * The MFP is the classic 68000-family companion chip: it bundles a
 * 16-source vectored interrupt controller, four timers, an eight-bit
 * parallel port with edge-triggered interrupts, and a USART into one
 * part.  On real boards it is the system interrupt controller, which is
 * how it is wired here.
 *
 * Implemented
 * -----------
 *   - All 24 registers at their datasheet offsets.
 *   - 16-channel interrupt controller: IERA/B, IPRA/B, ISRA/B, IMRA/B,
 *     correct priority order, vector generation from VR, and in-service
 *     inhibition of same-or-lower priority channels.
 *   - Timers A-D in delay mode with all seven prescaler ratios, reload
 *     from the data registers, and readable down-counters.
 *   - Timers A and B in event-count mode, counting active edges on the
 *     TAI/TBI inputs (shared with GPIP4 and GPIP3).
 *   - GPIP with DDR direction control and AER edge selection.
 *   - USART transmit and receive against a QEMU chardev, with the four
 *     USART interrupt channels.
 *
 * Deviations from the datasheet
 * -----------------------------
 *   - Interrupt acknowledge needed a small addition to the m68k CPU.
 *     Upstream QEMU notes in op_helper.c that "real hardware gets the
 *     interrupt vector via an IACK cycle at this point" and that no
 *     emulated hardware relied on it, so no callback existed.  A vectored
 *     controller does need it, so target/m68k gained an optional
 *     iack_handler which this device registers; that restores the real
 *     IACK-time behaviour (pending bit cleared, channel flagged in
 *     service in software-EOI mode).
 *   - Pulse-width measurement modes (timer control values 9-15) are
 *     treated as the equivalent delay mode and logged as unimplemented.
 *   - The USART models asynchronous character transfer only: sync modes,
 *     the sync character register, break generation and parity are
 *     stored but not acted upon.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "target/m68k/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/mc68901.h"

/* Prescaler divisors selected by the low three bits of a control register */
static const uint32_t mfp_prescale[8] = { 0, 4, 10, 16, 50, 64, 100, 200 };

/* Which interrupt channel each GPIP pin drives */
static const int mfp_gpip_channel[MFP_NUM_GPIP] = {
    MFP_INT_GPIP0, MFP_INT_GPIP1, MFP_INT_GPIP2, MFP_INT_GPIP3,
    MFP_INT_GPIP4, MFP_INT_GPIP5, MFP_INT_GPIP6, MFP_INT_GPIP7,
};

/* Which interrupt channel each timer drives */
static const int mfp_timer_channel[MFP_NUM_TIMERS] = {
    MFP_INT_TIMERA, MFP_INT_TIMERB, MFP_INT_TIMERC, MFP_INT_TIMERD,
};

/* Timers A and B can count edges on these GPIP pins (TAI / TBI) */
#define MFP_TAI_PIN 4
#define MFP_TBI_PIN 3

static inline bool mfp_chan_bit(uint8_t a, uint8_t b, int ch)
{
    return ch >= 8 ? (a >> (ch - 8)) & 1 : (b >> ch) & 1;
}

static inline void mfp_chan_set(uint8_t *a, uint8_t *b, int ch, bool v)
{
    uint8_t mask = ch >= 8 ? 1 << (ch - 8) : 1 << ch;
    uint8_t *reg = ch >= 8 ? a : b;

    if (v) {
        *reg |= mask;
    } else {
        *reg &= ~mask;
    }
}

/*
 * Recompute the interrupt the MFP is presenting to the CPU.
 *
 * The highest-numbered channel that is pending, enabled and unmasked
 * wins.  A channel that is in service inhibits itself and everything of
 * lower priority.
 */
static void mc68901_update_irq(MC68901State *s)
{
    M68kCPU *cpu = M68K_CPU(s->cpu);
    int ch, best = -1, inservice = -1;

    if (!cpu) {
        return;
    }

    for (ch = 15; ch >= 0; ch--) {
        if (mfp_chan_bit(s->isra, s->isrb, ch)) {
            inservice = ch;
            break;
        }
    }
    for (ch = 15; ch >= 0; ch--) {
        if (mfp_chan_bit(s->ipra, s->iprb, ch) &&
            mfp_chan_bit(s->iera, s->ierb, ch) &&
            mfp_chan_bit(s->imra, s->imrb, ch)) {
            best = ch;
            break;
        }
    }

    if (best >= 0 && inservice >= best) {
        best = -1;              /* inhibited by an in-service channel */
    }

    s->ack_channel = best;

    if (best >= 0) {
        m68k_set_irq_level(cpu, s->irq_level,
                           (s->vr & MFP_VR_BASE_MASK) | best);
    } else {
        m68k_set_irq_level(cpu, 0, 0);
    }
}

/*
 * Interrupt acknowledge.  Real silicon does this during the IACK bus
 * cycle: the pending bit of the channel being acknowledged is cleared,
 * and in software end-of-interrupt mode the channel is flagged in
 * service so that it and everything below it stay inhibited until the
 * handler clears the bit.
 */
static void mc68901_iack(void *opaque, int level)
{
    MC68901State *s = opaque;
    int ch = s->ack_channel;

    if (level != (int)s->irq_level || ch < 0) {
        return;
    }

    mfp_chan_set(&s->ipra, &s->iprb, ch, false);
    if (s->vr & MFP_VR_S) {
        mfp_chan_set(&s->isra, &s->isrb, ch, true);
    }
    mc68901_update_irq(s);
}

/*
 * Flag a channel as pending.  The datasheet is explicit that a disabled
 * channel does not set its pending bit at all, so the enable register is
 * checked here rather than only at delivery time.
 */
static void mc68901_raise(MC68901State *s, int ch)
{
    if (!mfp_chan_bit(s->iera, s->ierb, ch)) {
        return;
    }
    mfp_chan_set(&s->ipra, &s->iprb, ch, true);
    mc68901_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

/* Control-register value for a timer: 0 stopped, 1-7 delay, 8 event. */
static uint8_t mfp_timer_mode(MC68901State *s, int n)
{
    switch (n) {
    case 0: return s->tacr & 0x0f;
    case 1: return s->tbcr & 0x0f;
    case 2: return (s->tcdcr >> 4) & 0x07;
    default: return s->tcdcr & 0x07;
    }
}

static void mc68901_timer_recalc(MC68901State *s, int n)
{
    uint8_t mode = mfp_timer_mode(s, n);
    uint32_t reload = s->tdr[n] ? s->tdr[n] : 256;
    uint32_t div;
    int64_t now;

    if (mode == 0) {                        /* stopped */
        timer_del(s->timer[n]);
        s->timer_period[n] = 0;
        return;
    }
    if (mode == 8) {                        /* event count: no free-running */
        timer_del(s->timer[n]);
        s->timer_period[n] = 0;
        return;
    }
    if (mode > 8) {
        qemu_log_mask(LOG_UNIMP,
                      "mc68901: timer %d pulse-width mode %d not modelled, "
                      "treating as delay mode\n", n, mode);
        mode -= 8;
    }

    div = mfp_prescale[mode & 7];
    if (div == 0) {
        timer_del(s->timer[n]);
        s->timer_period[n] = 0;
        return;
    }

    /* One full pass of the counter, in nanoseconds. */
    s->timer_period[n] = muldiv64((int64_t)reload * div,
                                  NANOSECONDS_PER_SECOND, s->timer_freq);
    if (s->timer_period[n] <= 0) {
        s->timer_period[n] = 1;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->timer_next[n] = now + s->timer_period[n];
    timer_mod(s->timer[n], s->timer_next[n]);
}

static void mc68901_timer_expire(void *opaque, int n)
{
    MC68901State *s = opaque;

    mc68901_raise(s, mfp_timer_channel[n]);

    if (s->timer_period[n] > 0) {
        s->timer_next[n] += s->timer_period[n];
        timer_mod(s->timer[n], s->timer_next[n]);
    }
}

static void mc68901_timer_a(void *o) { mc68901_timer_expire(o, 0); }
static void mc68901_timer_b(void *o) { mc68901_timer_expire(o, 1); }
static void mc68901_timer_c(void *o) { mc68901_timer_expire(o, 2); }
static void mc68901_timer_d(void *o) { mc68901_timer_expire(o, 3); }

/* Current value of a timer's down counter. */
static uint8_t mc68901_timer_read(MC68901State *s, int n)
{
    uint32_t reload = s->tdr[n] ? s->tdr[n] : 256;
    int64_t now, remain, tick;
    uint32_t cnt;

    if (mfp_timer_mode(s, n) == 8) {        /* event count mode */
        return s->timer_event[n];
    }
    if (s->timer_period[n] <= 0) {          /* stopped: reads the reload */
        return s->tdr[n];
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    remain = s->timer_next[n] - now;
    if (remain < 0) {
        remain = 0;
    }
    tick = s->timer_period[n] / reload;
    if (tick <= 0) {
        return s->tdr[n];
    }
    cnt = (remain + tick - 1) / tick;
    if (cnt == 0 || cnt > reload) {
        cnt = reload;
    }
    return (uint8_t)cnt;
}

/* An active edge arrived on TAI or TBI while that timer is in event mode. */
static void mc68901_timer_event_tick(MC68901State *s, int n)
{
    if (mfp_timer_mode(s, n) != 8) {
        return;
    }
    if (s->timer_event[n] == 0) {
        s->timer_event[n] = s->tdr[n] ? s->tdr[n] : 255;
    }
    s->timer_event[n]--;
    if (s->timer_event[n] == 0) {
        s->timer_event[n] = s->tdr[n] ? s->tdr[n] : 255;
        mc68901_raise(s, mfp_timer_channel[n]);
    }
}

/* ------------------------------------------------------------------ */
/* GPIP                                                                */
/* ------------------------------------------------------------------ */

static void mc68901_gpip_set(void *opaque, int n, int level)
{
    MC68901State *s = opaque;
    uint8_t mask = 1 << n;
    bool was = (s->gpip_in & mask) != 0;
    bool now = level != 0;

    if (was == now) {
        return;
    }
    if (now) {
        s->gpip_in |= mask;
    } else {
        s->gpip_in &= ~mask;
    }

    /* AER bit set => interrupt on the low-to-high transition. */
    if (now == ((s->aer & mask) != 0)) {
        mc68901_raise(s, mfp_gpip_channel[n]);

        if (n == MFP_TAI_PIN) {
            mc68901_timer_event_tick(s, 0);
        }
        if (n == MFP_TBI_PIN) {
            mc68901_timer_event_tick(s, 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* USART                                                               */
/* ------------------------------------------------------------------ */

static int mc68901_can_receive(void *opaque)
{
    MC68901State *s = opaque;

    return (s->rsr & MFP_RSR_RE) && !(s->rsr & MFP_RSR_BF);
}

static void mc68901_receive(void *opaque, const uint8_t *buf, int size)
{
    MC68901State *s = opaque;

    if (!(s->rsr & MFP_RSR_RE)) {
        return;
    }
    if (s->rsr & MFP_RSR_BF) {
        s->rsr |= MFP_RSR_OE;
        mc68901_raise(s, MFP_INT_RCV_ERR);
        return;
    }
    s->udr_rx = buf[0];
    s->rsr |= MFP_RSR_BF;
    mc68901_raise(s, MFP_INT_RCV_FULL);
}

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

static uint64_t mc68901_read(void *opaque, hwaddr addr, unsigned size)
{
    MC68901State *s = opaque;
    unsigned reg = addr >> s->regshift;

    switch (reg) {
    case MFP_GPIP:
        /* Output pins read back what was written, input pins read the pin. */
        return (s->gpip_out & s->ddr) | (s->gpip_in & ~s->ddr);
    case MFP_AER:   return s->aer;
    case MFP_DDR:   return s->ddr;
    case MFP_IERA:  return s->iera;
    case MFP_IERB:  return s->ierb;
    case MFP_IPRA:  return s->ipra;
    case MFP_IPRB:  return s->iprb;
    case MFP_ISRA:  return s->isra;
    case MFP_ISRB:  return s->isrb;
    case MFP_IMRA:  return s->imra;
    case MFP_IMRB:  return s->imrb;
    case MFP_VR:    return s->vr;
    case MFP_TACR:  return s->tacr;
    case MFP_TBCR:  return s->tbcr;
    case MFP_TCDCR: return s->tcdcr;
    case MFP_TADR:  return mc68901_timer_read(s, 0);
    case MFP_TBDR:  return mc68901_timer_read(s, 1);
    case MFP_TCDR:  return mc68901_timer_read(s, 2);
    case MFP_TDDR:  return mc68901_timer_read(s, 3);
    case MFP_SCR:   return s->scr;
    case MFP_UCR:   return s->ucr;
    case MFP_RSR:   return s->rsr;
    case MFP_TSR:   return s->tsr;
    case MFP_UDR:
        s->rsr &= ~MFP_RSR_BF;
        qemu_chr_fe_accept_input(&s->chr);
        return s->udr_rx;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mc68901: read from invalid register %u\n", reg);
        return 0;
    }
}

static void mc68901_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    MC68901State *s = opaque;
    unsigned reg = addr >> s->regshift;
    uint8_t v = val & 0xff;

    switch (reg) {
    case MFP_GPIP:
        s->gpip_out = v;
        break;
    case MFP_AER:
        s->aer = v;
        break;
    case MFP_DDR:
        s->ddr = v;
        break;

    case MFP_IERA:
        s->iera = v;
        /* Disabling a channel also clears any interrupt it had pending. */
        s->ipra &= v;
        mc68901_update_irq(s);
        break;
    case MFP_IERB:
        s->ierb = v;
        s->iprb &= v;
        mc68901_update_irq(s);
        break;

    /*
     * Writing a zero to a pending or in-service bit clears it; writing a
     * one leaves it alone.  This is how the datasheet specifies software
     * clears, and it means a handler can clear exactly its own bit with
     * a single write of ~mask.
     */
    case MFP_IPRA:
        s->ipra &= v;
        mc68901_update_irq(s);
        break;
    case MFP_IPRB:
        s->iprb &= v;
        mc68901_update_irq(s);
        break;
    case MFP_ISRA:
        s->isra &= v;
        mc68901_update_irq(s);
        break;
    case MFP_ISRB:
        s->isrb &= v;
        mc68901_update_irq(s);
        break;

    case MFP_IMRA:
        s->imra = v;
        mc68901_update_irq(s);
        break;
    case MFP_IMRB:
        s->imrb = v;
        mc68901_update_irq(s);
        break;
    case MFP_VR:
        s->vr = v;
        /* Leaving software-EOI mode clears the in-service registers. */
        if (!(v & MFP_VR_S)) {
            s->isra = 0;
            s->isrb = 0;
        }
        mc68901_update_irq(s);
        break;

    case MFP_TACR:
        s->tacr = v;
        mc68901_timer_recalc(s, 0);
        break;
    case MFP_TBCR:
        s->tbcr = v;
        mc68901_timer_recalc(s, 1);
        break;
    case MFP_TCDCR:
        s->tcdcr = v;
        mc68901_timer_recalc(s, 2);
        mc68901_timer_recalc(s, 3);
        break;

    case MFP_TADR:
    case MFP_TBDR:
    case MFP_TCDR:
    case MFP_TDDR: {
        int n = reg - MFP_TADR;
        s->tdr[n] = v;
        s->timer_event[n] = v;
        mc68901_timer_recalc(s, n);
        break;
    }

    case MFP_SCR:
        s->scr = v;
        break;
    case MFP_UCR:
        s->ucr = v;
        break;
    case MFP_RSR:
        /* RE is writable; the status flags are not set by software. */
        s->rsr = (s->rsr & ~MFP_RSR_RE) | (v & MFP_RSR_RE);
        if (!(v & MFP_RSR_RE)) {
            s->rsr &= ~(MFP_RSR_BF | MFP_RSR_OE);
        }
        break;
    case MFP_TSR:
        s->tsr = (s->tsr & ~MFP_TSR_TE) | (v & MFP_TSR_TE);
        if (v & MFP_TSR_TE) {
            s->tsr |= MFP_TSR_BE;
            mc68901_raise(s, MFP_INT_XMIT_EMPTY);
        } else {
            s->tsr &= ~MFP_TSR_BE;
        }
        break;
    case MFP_UDR:
        if (s->tsr & MFP_TSR_TE) {
            qemu_chr_fe_write_all(&s->chr, &v, 1);
            /* Transmission is instantaneous, so the buffer is empty again. */
            s->tsr |= MFP_TSR_BE | MFP_TSR_END;
            mc68901_raise(s, MFP_INT_XMIT_EMPTY);
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mc68901: write to invalid register %u\n", reg);
        return;
    }
}

static const MemoryRegionOps mc68901_ops = {
    .read = mc68901_read,
    .write = mc68901_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* ------------------------------------------------------------------ */
/* QOM plumbing                                                        */
/* ------------------------------------------------------------------ */

static void mc68901_reset_hold(Object *obj, ResetType type)
{
    MC68901State *s = MC68901(obj);
    int i;

    s->gpip_in = 0;
    s->gpip_out = 0;
    s->aer = 0;
    s->ddr = 0;
    s->iera = s->ierb = 0;
    s->ipra = s->iprb = 0;
    s->isra = s->isrb = 0;
    s->imra = s->imrb = 0;
    s->vr = 0;
    s->tacr = s->tbcr = s->tcdcr = 0;
    s->scr = s->ucr = 0;
    s->rsr = 0;
    s->tsr = MFP_TSR_BE;
    s->udr_rx = 0;
    s->ack_channel = -1;

    for (i = 0; i < MFP_NUM_TIMERS; i++) {
        s->tdr[i] = 0;
        s->timer_event[i] = 0;
        s->timer_period[i] = 0;
        s->timer_next[i] = 0;
        timer_del(s->timer[i]);
    }

    mc68901_update_irq(s);
}

static void mc68901_realize(DeviceState *dev, Error **errp)
{
    MC68901State *s = MC68901(dev);

    if (s->irq_level < 1 || s->irq_level > 7) {
        error_setg(errp, "mc68901: irq-level must be between 1 and 7");
        return;
    }
    if (s->timer_freq == 0) {
        error_setg(errp, "mc68901: timer-frequency must be non-zero");
        return;
    }

    qemu_chr_fe_set_handlers(&s->chr, mc68901_can_receive, mc68901_receive,
                             NULL, NULL, s, NULL, true);

    if (s->cpu) {
        m68k_set_iack_handler(M68K_CPU(s->cpu), mc68901_iack, s);
    }
}

static void mc68901_init(Object *obj)
{
    MC68901State *s = MC68901(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &mc68901_ops, s, "mc68901",
                          MFP_NUM_REGS << s->regshift);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in(DEVICE(obj), mc68901_gpip_set, MFP_NUM_GPIP);

    s->timer[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mc68901_timer_a, s);
    s->timer[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mc68901_timer_b, s);
    s->timer[2] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mc68901_timer_c, s);
    s->timer[3] = timer_new_ns(QEMU_CLOCK_VIRTUAL, mc68901_timer_d, s);
}

static const VMStateDescription vmstate_mc68901 = {
    .name = "mc68901",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(gpip_in, MC68901State),
        VMSTATE_UINT8(gpip_out, MC68901State),
        VMSTATE_UINT8(aer, MC68901State),
        VMSTATE_UINT8(ddr, MC68901State),
        VMSTATE_UINT8(iera, MC68901State),
        VMSTATE_UINT8(ierb, MC68901State),
        VMSTATE_UINT8(ipra, MC68901State),
        VMSTATE_UINT8(iprb, MC68901State),
        VMSTATE_UINT8(isra, MC68901State),
        VMSTATE_UINT8(isrb, MC68901State),
        VMSTATE_UINT8(imra, MC68901State),
        VMSTATE_UINT8(imrb, MC68901State),
        VMSTATE_UINT8(vr, MC68901State),
        VMSTATE_UINT8(tacr, MC68901State),
        VMSTATE_UINT8(tbcr, MC68901State),
        VMSTATE_UINT8(tcdcr, MC68901State),
        VMSTATE_UINT8_ARRAY(tdr, MC68901State, MFP_NUM_TIMERS),
        VMSTATE_UINT8_ARRAY(timer_event, MC68901State, MFP_NUM_TIMERS),
        VMSTATE_UINT8(scr, MC68901State),
        VMSTATE_UINT8(ucr, MC68901State),
        VMSTATE_UINT8(rsr, MC68901State),
        VMSTATE_UINT8(tsr, MC68901State),
        VMSTATE_UINT8(udr_rx, MC68901State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mc68901_properties[] = {
    DEFINE_PROP_LINK("m68k-cpu", MC68901State, cpu, TYPE_M68K_CPU, ArchCPU *),
    DEFINE_PROP_UINT32("regshift", MC68901State, regshift, 0),
    DEFINE_PROP_UINT32("irq-level", MC68901State, irq_level, 6),
    DEFINE_PROP_UINT64("timer-frequency", MC68901State, timer_freq, 2457600),
    DEFINE_PROP_CHR("chardev", MC68901State, chr),
};

static void mc68901_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->realize = mc68901_realize;
    dc->vmsd = &vmstate_mc68901;
    dc->desc = "Motorola MC68901 Multi-Function Peripheral";
    device_class_set_props(dc, mc68901_properties);
    rc->phases.hold = mc68901_reset_hold;
}

static const TypeInfo mc68901_info = {
    .name          = TYPE_MC68901,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MC68901State),
    .instance_init = mc68901_init,
    .class_init    = mc68901_class_init,
};

static void mc68901_register_types(void)
{
    type_register_static(&mc68901_info);
}

type_init(mc68901_register_types)
