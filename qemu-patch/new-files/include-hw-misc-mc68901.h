/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Motorola MC68901 Multi-Function Peripheral (MFP)
 *
 * Reference: Motorola MC68901 Multi-Function Peripheral datasheet.
 */

#ifndef HW_MISC_MC68901_H
#define HW_MISC_MC68901_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_MC68901 "mc68901"
OBJECT_DECLARE_SIMPLE_TYPE(MC68901State, MC68901)

/* Register offsets, in register numbers (scaled by the "regshift" property) */
#define MFP_GPIP    0x00    /* General Purpose I/O                     */
#define MFP_AER     0x01    /* Active Edge Register                    */
#define MFP_DDR     0x02    /* Data Direction Register                 */
#define MFP_IERA    0x03    /* Interrupt Enable A                      */
#define MFP_IERB    0x04    /* Interrupt Enable B                      */
#define MFP_IPRA    0x05    /* Interrupt Pending A                     */
#define MFP_IPRB    0x06    /* Interrupt Pending B                     */
#define MFP_ISRA    0x07    /* Interrupt In-Service A                  */
#define MFP_ISRB    0x08    /* Interrupt In-Service B                  */
#define MFP_IMRA    0x09    /* Interrupt Mask A                        */
#define MFP_IMRB    0x0a    /* Interrupt Mask B                        */
#define MFP_VR      0x0b    /* Vector Register                         */
#define MFP_TACR    0x0c    /* Timer A Control                         */
#define MFP_TBCR    0x0d    /* Timer B Control                         */
#define MFP_TCDCR   0x0e    /* Timers C and D Control                  */
#define MFP_TADR    0x0f    /* Timer A Data                            */
#define MFP_TBDR    0x10    /* Timer B Data                            */
#define MFP_TCDR    0x11    /* Timer C Data                            */
#define MFP_TDDR    0x12    /* Timer D Data                            */
#define MFP_SCR     0x13    /* Sync Character                          */
#define MFP_UCR     0x14    /* USART Control                           */
#define MFP_RSR     0x15    /* Receiver Status                         */
#define MFP_TSR     0x16    /* Transmitter Status                      */
#define MFP_UDR     0x17    /* USART Data                              */

#define MFP_NUM_REGS 0x18

/*
 * Interrupt channels, 0 (lowest priority) to 15 (highest).
 * Channels 0-7 live in the "B" registers, 8-15 in the "A" registers.
 */
#define MFP_INT_GPIP0       0
#define MFP_INT_GPIP1       1
#define MFP_INT_GPIP2       2
#define MFP_INT_GPIP3       3
#define MFP_INT_TIMERD      4
#define MFP_INT_TIMERC      5
#define MFP_INT_GPIP4       6
#define MFP_INT_GPIP5       7
#define MFP_INT_TIMERB      8
#define MFP_INT_XMIT_ERR    9
#define MFP_INT_XMIT_EMPTY  10
#define MFP_INT_RCV_ERR     11
#define MFP_INT_RCV_FULL    12
#define MFP_INT_TIMERA      13
#define MFP_INT_GPIP6       14
#define MFP_INT_GPIP7       15

/* Vector Register bits */
#define MFP_VR_S            0x08    /* 1 = software end-of-interrupt    */
#define MFP_VR_BASE_MASK    0xf0

/* Transmitter Status Register bits */
#define MFP_TSR_TE          0x01    /* transmitter enable               */
#define MFP_TSR_END         0x10    /* end of transmission              */
#define MFP_TSR_BE          0x80    /* buffer empty                     */

/* Receiver Status Register bits */
#define MFP_RSR_RE          0x01    /* receiver enable                  */
#define MFP_RSR_OE          0x40    /* overrun error                    */
#define MFP_RSR_BF          0x80    /* buffer full                      */

#define MFP_NUM_TIMERS      4
#define MFP_NUM_GPIP        8

struct MC68901State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharFrontend chr;
    ArchCPU *cpu;

    /* properties */
    uint32_t regshift;      /* 0 = byte spacing, 1 = 2-byte (Atari style) */
    uint32_t irq_level;     /* which 68k IPL this MFP drives              */
    uint64_t timer_freq;    /* XTAL1 timer clock in Hz                    */

    /* registers */
    uint8_t gpip_in;        /* state of the input pins                    */
    uint8_t gpip_out;       /* value written for pins programmed as output*/
    uint8_t aer, ddr;
    uint8_t iera, ierb;
    uint8_t ipra, iprb;
    uint8_t isra, isrb;
    uint8_t imra, imrb;
    uint8_t vr;
    uint8_t tacr, tbcr, tcdcr;
    uint8_t tdr[MFP_NUM_TIMERS];    /* A, B, C, D reload values           */
    uint8_t scr, ucr, rsr, tsr, udr_rx;

    int ack_channel;        /* channel currently presented to the CPU     */

    /* timer bookkeeping */
    QEMUTimer *timer[MFP_NUM_TIMERS];
    int64_t timer_next[MFP_NUM_TIMERS];     /* ns of next expiry          */
    int64_t timer_period[MFP_NUM_TIMERS];   /* ns for a full reload cycle */
    uint8_t timer_event[MFP_NUM_TIMERS];    /* event-mode down counter    */
};

#endif /* HW_MISC_MC68901_H */
