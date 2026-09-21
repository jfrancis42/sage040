/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sage040.h - hardware definitions for the Sage040 machine.
 *
 * Every register here comes from a real datasheet:
 *   NS16550A UART   - TI/NS PC16550D datasheet
 *   ATA taskfile    - T13 ATA/ATAPI specification
 *   SMSC LAN91C111  - SMSC LAN91C111 datasheet
 *   MC68040         - Motorola M68040 User's Manual
 */
#ifndef SAGE040_H
#define SAGE040_H

#include "types.h"

#define MMIO8(a)   (*(volatile u8  *)(a))
#define MMIO16(a)  (*(volatile u16 *)(a))
#define MMIO32(a)  (*(volatile u32 *)(a))

/* ---------------------------------------------------------------- */
/* NS16550A UART at 0xff000000, registers byte-spaced (regshift 0)   */
/* ---------------------------------------------------------------- */
#define UART_BASE   0xff000000UL

#define UART_RBR    (UART_BASE + 0)   /* R: receive buffer            */
#define UART_THR    (UART_BASE + 0)   /* W: transmit holding          */
#define UART_DLL    (UART_BASE + 0)   /* R/W when DLAB=1: divisor low */
#define UART_IER    (UART_BASE + 1)   /* R/W: interrupt enable        */
#define UART_DLM    (UART_BASE + 1)   /* R/W when DLAB=1: divisor high*/
#define UART_IIR    (UART_BASE + 2)   /* R: interrupt ident           */
#define UART_FCR    (UART_BASE + 2)   /* W: FIFO control              */
#define UART_LCR    (UART_BASE + 3)   /* R/W: line control            */
#define UART_MCR    (UART_BASE + 4)   /* R/W: modem control           */
#define UART_LSR    (UART_BASE + 5)   /* R: line status               */
#define UART_MSR    (UART_BASE + 6)   /* R: modem status              */
#define UART_SCR    (UART_BASE + 7)   /* R/W: scratch                 */

#define LSR_DR      0x01              /* data ready                   */
#define LSR_THRE    0x20              /* transmit holding empty       */
#define LSR_TEMT    0x40              /* transmitter empty            */
#define LCR_DLAB    0x80              /* divisor latch access         */
#define LCR_8N1     0x03              /* 8 bits, no parity, 1 stop    */
#define MCR_LOOP    0x10              /* local loopback               */
#define MCR_DTR     0x01
#define MCR_RTS     0x02
#define FCR_ENABLE  0x01
#define FCR_CLR_RX  0x02
#define FCR_CLR_TX  0x04

/* ---------------------------------------------------------------- */
/* ATA taskfile at 0xff100000 (command block), 0xff101000 (control)  */
/*                                                                    */
/* Offset 0 of the command block is the 16-bit data register; offsets */
/* 1..7 are the taskfile registers, exactly as ATA numbers them.      */
/* ---------------------------------------------------------------- */
#define ATA_CMD_BASE 0xff100000UL
#define ATA_CTL_BASE 0xff101000UL

#define ATA_DATA     (ATA_CMD_BASE + 0)   /* 16-bit                   */
#define ATA_ERROR    (ATA_CMD_BASE + 1)   /* R                        */
#define ATA_FEATURE  (ATA_CMD_BASE + 1)   /* W                        */
#define ATA_NSECT    (ATA_CMD_BASE + 2)
#define ATA_LBAL     (ATA_CMD_BASE + 3)
#define ATA_LBAM     (ATA_CMD_BASE + 4)
#define ATA_LBAH     (ATA_CMD_BASE + 5)
#define ATA_DEVICE   (ATA_CMD_BASE + 6)
#define ATA_STATUS   (ATA_CMD_BASE + 7)   /* R                        */
#define ATA_COMMAND  (ATA_CMD_BASE + 7)   /* W                        */
#define ATA_ALTSTAT  (ATA_CTL_BASE + 0)   /* R                        */
#define ATA_DEVCTL   (ATA_CTL_BASE + 0)   /* W                        */

#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30

#define ATA_DEV_LBA        0xE0          /* LBA mode, master         */

/* ---------------------------------------------------------------- */
/* SMSC LAN91C111 at 0xff200000                                      */
/*                                                                    */
/* A 16-byte window.  Offset 14 (0x0e) is the bank select register,   */
/* visible from every bank.  All other offsets change meaning with    */
/* the selected bank.                                                 */
/* ---------------------------------------------------------------- */
#define SMC_BASE     0xff200000UL
#define SMC_REG(o)   (SMC_BASE + (o))
#define SMC_BANKSEL  SMC_REG(14)

/* Bank 0 */
#define SMC_B0_TCR      SMC_REG(0)    /* transmit control            */
#define SMC_B0_EPHSR    SMC_REG(2)    /* EPH status                  */
#define SMC_B0_RCR      SMC_REG(4)    /* receive control             */
#define SMC_B0_COUNTER  SMC_REG(6)
#define SMC_B0_MIR      SMC_REG(8)    /* memory information          */
#define SMC_B0_RPCR     SMC_REG(10)

/* Bank 1 */
#define SMC_B1_CONFIG   SMC_REG(0)
#define SMC_B1_BASE     SMC_REG(2)
#define SMC_B1_IA0      SMC_REG(4)    /* MAC address bytes 0..5      */
#define SMC_B1_GENERAL  SMC_REG(10)
#define SMC_B1_CONTROL  SMC_REG(12)

/* Bank 2 */
#define SMC_B2_MMUCMD   SMC_REG(0)
#define SMC_B2_PNR      SMC_REG(2)    /* packet number / alloc result*/
#define SMC_B2_FIFO     SMC_REG(4)
#define SMC_B2_PTR      SMC_REG(6)
#define SMC_B2_DATA     SMC_REG(8)
#define SMC_B2_INT      SMC_REG(12)   /* interrupt status / mask     */

/* Bank 3 */
#define SMC_B3_MT       SMC_REG(0)    /* multicast table             */
#define SMC_B3_MGMT     SMC_REG(8)
#define SMC_B3_REV      SMC_REG(10)   /* chip revision               */
#define SMC_B3_ERCV     SMC_REG(12)

#define SMC_MMU_ALLOC_TX  0x0020
#define SMC_MMU_RESET     0x0040
#define SMC_MMU_REMOVE_RX 0x0060
#define SMC_MMU_RELEASE   0x0080
#define SMC_MMU_ENQUEUE   0x00C0

#define SMC_PTR_RCV       0x8000
#define SMC_PTR_AUTOINC   0x4000
#define SMC_PTR_READ      0x2000

#define SMC_INT_RCV       0x01
#define SMC_INT_TX        0x02
#define SMC_INT_TX_EMPTY  0x04
#define SMC_INT_ALLOC     0x08


/* ---------------------------------------------------------------- */
/* Motorola MC68901 MFP at 0xff300000, byte-spaced registers         */
/*                                                                    */
/* Reference: MC68901 Multi-Function Peripheral datasheet.            */
/* ---------------------------------------------------------------- */
#define MFP_BASE     0xff300000UL

#define MFP_GPIP     (MFP_BASE + 0x00)   /* general purpose I/O       */
#define MFP_AER      (MFP_BASE + 0x01)   /* active edge               */
#define MFP_DDR      (MFP_BASE + 0x02)   /* data direction            */
#define MFP_IERA     (MFP_BASE + 0x03)   /* interrupt enable A        */
#define MFP_IERB     (MFP_BASE + 0x04)
#define MFP_IPRA     (MFP_BASE + 0x05)   /* interrupt pending A       */
#define MFP_IPRB     (MFP_BASE + 0x06)
#define MFP_ISRA     (MFP_BASE + 0x07)   /* in-service A              */
#define MFP_ISRB     (MFP_BASE + 0x08)
#define MFP_IMRA     (MFP_BASE + 0x09)   /* interrupt mask A          */
#define MFP_IMRB     (MFP_BASE + 0x0a)
#define MFP_VR       (MFP_BASE + 0x0b)   /* vector register           */
#define MFP_TACR     (MFP_BASE + 0x0c)   /* timer A control           */
#define MFP_TBCR     (MFP_BASE + 0x0d)
#define MFP_TCDCR    (MFP_BASE + 0x0e)   /* timers C and D control    */
#define MFP_TADR     (MFP_BASE + 0x0f)   /* timer A data              */
#define MFP_TBDR     (MFP_BASE + 0x10)
#define MFP_TCDR     (MFP_BASE + 0x11)
#define MFP_TDDR     (MFP_BASE + 0x12)
#define MFP_SCR      (MFP_BASE + 0x13)   /* sync character            */
#define MFP_UCR      (MFP_BASE + 0x14)   /* USART control             */
#define MFP_RSR      (MFP_BASE + 0x15)   /* receiver status           */
#define MFP_TSR      (MFP_BASE + 0x16)   /* transmitter status        */
#define MFP_UDR      (MFP_BASE + 0x17)   /* USART data                */

/* Interrupt channels, 0 = lowest priority, 15 = highest. */
#define MFPCH_GPIP0        0
#define MFPCH_GPIP1        1     /* 8042 keyboard on this board         */
#define MFPCH_GPIP2        2     /* M48T59 alarm/watchdog on this board */
#define MFPCH_GPIP3        3     /* LAN91C111 on this board; also TBI */
#define MFPCH_TIMERD       4
#define MFPCH_TIMERC       5
#define MFPCH_GPIP4        6     /* ATA on this board; also TAI       */
#define MFPCH_GPIP5        7     /* NS16550A on this board            */
#define MFPCH_TIMERB       8
#define MFPCH_XMIT_ERR     9
#define MFPCH_XMIT_EMPTY  10
#define MFPCH_RCV_ERR     11
#define MFPCH_RCV_FULL    12
#define MFPCH_TIMERA      13
#define MFPCH_GPIP6       14
#define MFPCH_GPIP7       15

/* Board wiring: which GPIP pin each peripheral drives. */
#define MFP_PIN_UART      5
#define MFP_PIN_ATA       4
#define MFP_PIN_NET       3

/* Vector register bits */
#define MFP_VR_S          0x08   /* software end-of-interrupt mode    */

/* Timer control values (low 3 bits select the prescaler) */
#define MFP_TC_STOP       0
#define MFP_TC_DIV4       1
#define MFP_TC_DIV10      2
#define MFP_TC_DIV16      3
#define MFP_TC_DIV50      4
#define MFP_TC_DIV64      5
#define MFP_TC_DIV100     6
#define MFP_TC_DIV200     7
#define MFP_TC_EVENT      8      /* timers A and B only               */

/* USART status bits */
#define MFP_TSR_TE        0x01
#define MFP_TSR_END       0x10
#define MFP_TSR_BE        0x80
#define MFP_RSR_RE        0x01
#define MFP_RSR_OE        0x40
#define MFP_RSR_BF        0x80

/* The MFP drives IPL 6 on this board, and its XTAL1 runs at the classic
 * 2.4576 MHz -- so a timer's rate is 2457600 / prescaler / reload. */
#define MFP_IPL           6
#define MFP_XTAL1         2457600UL

/* ---------------------------------------------------------------- */
/* MFP interrupt plumbing (mfp.c)                                    */
/*                                                                    */
/* Part of the test support library, not of the hardware.  The kernel */
/* has its own driver and defines SAGE040_NO_TESTLIB to keep these    */
/* out of its way: they collide by name with what a driver naturally  */
/* calls its own helpers, and a collision here is a compile error in  */
/* a file that did nothing wrong.                                     */
/* ---------------------------------------------------------------- */
#ifndef SAGE040_NO_TESTLIB
extern volatile int mfp_irq_count;      /* interrupts taken          */
extern volatile int mfp_last_vector;    /* vector of the last one    */
extern volatile int mfp_last_channel;   /* channel of the last one   */
extern volatile int mfp_vec_count[16];  /* per-channel counters      */
extern volatile int mfp_isr_saw_inservice; /* ISR set inside handler? */
extern volatile int mfp_suppress_eoi;   /* hold channel in service    */

void mfp_install_vectors(u8 vr_base);   /* point all 16 at the stub  */
void mfp_reset_counts(void);
void mfp_enable(int ch);                /* IER + IMR bits for ch     */
void mfp_disable(int ch);
void mfp_clear_pending(int ch);
int  mfp_is_pending(int ch);
int  mfp_in_service(int ch);
void mfp_eoi(int ch);
void mfp_set_ipl(int level);
u16  mfp_get_sr(void);
#endif /* SAGE040_NO_TESTLIB */

/* ---------------------------------------------------------------- */
/* Silicon Motion SM501 video                                        */
/*                                                                    */
/* Reference: Silicon Motion SM501 datasheet.                         */
/* ---------------------------------------------------------------- */
#define SM501_VRAM      0xf0000000UL
#define SM501_VRAM_SIZE 0x01000000UL    /* 16 MiB */
#define SM501_MMIO      0xff400000UL

#define SM501_SYSTEM_CONTROL  (SM501_MMIO + 0x000000)
#define SM501_MISC_CONTROL    (SM501_MMIO + 0x000004)
#define SM501_DRAM_CONTROL    (SM501_MMIO + 0x000010)
#define SM501_DEVICEID        (SM501_MMIO + 0x000060)

#define SM501_DEVICEID_VALUE  0x050100A0UL

/* Display controller block, and the panel registers within it. */
#define SM501_DC              (SM501_MMIO + 0x080000)
#define SM501_PANEL_CONTROL   (SM501_DC + 0x000)
#define SM501_PANEL_FB_ADDR   (SM501_DC + 0x00C)
#define SM501_PANEL_FB_OFFSET (SM501_DC + 0x010)
#define SM501_PANEL_FB_WIDTH  (SM501_DC + 0x014)
#define SM501_PANEL_FB_HEIGHT (SM501_DC + 0x018)
#define SM501_PANEL_TL_LOC    (SM501_DC + 0x01C)
#define SM501_PANEL_BR_LOC    (SM501_DC + 0x020)
#define SM501_PANEL_H_TOTAL   (SM501_DC + 0x024)
#define SM501_PANEL_V_TOTAL   (SM501_DC + 0x02C)
#define SM501_PANEL_PALETTE   (SM501_DC + 0x400)

#define SM501_PC_ENABLE       (1UL << 2)    /* display enable          */
#define SM501_PC_8BPP         (0UL << 0)
#define SM501_PC_16BPP        (1UL << 0)
#define SM501_PC_32BPP        (2UL << 0)
#define SM501_PC_FPEN         (1UL << 27)
#define SM501_PC_VDD          (1UL << 24)
#define SM501_PC_DATA         (1UL << 25)

/*
 * 2D drawing engine.  Same rules as the other SM501 registers: 32-bit
 * accesses only, little-endian, so use SM501_RD / SM501_WR.
 * Writing CONTROL with the start bit set performs the operation.
 */
#define SM501_2D              (SM501_MMIO + 0x100000)
#define SM501_2D_SOURCE       (SM501_2D + 0x00)
#define SM501_2D_DEST         (SM501_2D + 0x04)   /* (x << 16) | y      */
#define SM501_2D_DIMENSION    (SM501_2D + 0x08)   /* (w << 16) | h      */
#define SM501_2D_CONTROL      (SM501_2D + 0x0C)
#define SM501_2D_PITCH        (SM501_2D + 0x10)   /* (dst << 16) | src  */
#define SM501_2D_FOREGROUND   (SM501_2D + 0x14)
#define SM501_2D_BACKGROUND   (SM501_2D + 0x18)
#define SM501_2D_STRETCH      (SM501_2D + 0x1C)   /* bits 20-21 = format */
#define SM501_2D_CLIP_TL      (SM501_2D + 0x2C)
#define SM501_2D_CLIP_BR      (SM501_2D + 0x30)
#define SM501_2D_WINDOW_W     (SM501_2D + 0x3C)
#define SM501_2D_SRC_BASE     (SM501_2D + 0x40)
#define SM501_2D_DST_BASE     (SM501_2D + 0x44)
#define SM501_2D_STATUS       (SM501_2D + 0x50)

#define SM501_2D_CMD_BITBLT   (0UL << 16)
#define SM501_2D_CMD_RECTFILL (1UL << 16)
#define SM501_2D_START        (1UL << 31)
#define SM501_2D_FMT_8BPP     (0UL << 20)
#define SM501_2D_FMT_16BPP    (1UL << 20)
#define SM501_2D_FMT_32BPP    (2UL << 20)

/*
 * The SM501's control registers are LITTLE-endian and accept 32-bit
 * accesses only - it is a PC-era part.  Its video memory, by contrast,
 * is ordinary RAM and reads back in the CPU's own big-endian order.
 * Use these accessors for registers and plain MMIO32 for the framebuffer.
 */
static inline u32 sm501_bswap32(u32 v)
{
    return ((v & 0x000000ffUL) << 24) | ((v & 0x0000ff00UL) << 8) |
           ((v & 0x00ff0000UL) >> 8)  | ((v & 0xff000000UL) >> 24);
}

#define SM501_RD(a)     sm501_bswap32(MMIO32(a))
#define SM501_WR(a, v)  (MMIO32(a) = sm501_bswap32((u32)(v)))

/* ---------------------------------------------------------------- */
/* Intel 8042 keyboard controller at 0xff700000                      */
/*                                                                    */
/* Reference: Intel 8042 datasheet; IBM PC/AT Technical Reference.    */
/*                                                                    */
/* The PC put the data port at 0x60 and status/command at 0x64. Here  */
/* the controller decodes one address line, so they are adjacent:     */
/* even is data, odd is status on read and command on write. That is  */
/* the same pair with the gap taken out.                              */
/* ---------------------------------------------------------------- */
#define KBD_BASE        0xff700000UL
#define KBD_DATA        (KBD_BASE + 0)   /* R/W                       */
#define KBD_STATUS      (KBD_BASE + 1)   /* R                         */
#define KBD_COMMAND     (KBD_BASE + 1)   /* W                         */

#define KBD_STAT_OBF        0x01   /* a byte is waiting to be read    */
#define KBD_STAT_IBF        0x02   /* the controller is still busy    */
#define KBD_STAT_SYS        0x04
#define KBD_STAT_CMD        0x08
#define KBD_STAT_AUX        0x20   /* the byte came from the mouse    */

/* Commands, written to KBD_COMMAND. */
#define KBD_CCMD_READ_MODE  0x20
#define KBD_CCMD_WRITE_MODE 0x60
#define KBD_CCMD_SELF_TEST  0xAA
#define KBD_CCMD_KBD_TEST   0xAB
#define KBD_CCMD_KBD_DISABLE 0xAD
#define KBD_CCMD_KBD_ENABLE 0xAE
/*
 * Pulse the output port's low line, which on a PC is wired to the CPU's
 * RESET. The keyboard controller resetting the processor is the most
 * famous accident in the IBM PC's design -- there was a spare open-drain
 * output on the 8042 and nowhere else to put the signal -- and it
 * outlived every other part of that machine.
 *
 * It is the machine's only way to stop, so `shutdown` uses it. Under the
 * emulator with -no-reboot a guest reset ends the process, which is
 * exactly what is wanted; on real hardware the board would come back up.
 */
#define KBD_CCMD_RESET      0xFE

/* Bits of the command byte. */
#define KBD_MODE_KBD_INT    0x01   /* interrupt when a byte arrives   */
#define KBD_MODE_SYS        0x04
#define KBD_MODE_DISABLE_KBD 0x10
#define KBD_MODE_TRANSLATE  0x40   /* set 2 in, set 1 out             */

#define KBD_REPLY_ACK       0xFA
#define KBD_SELF_TEST_OK    0x55

/* The keyboard interrupt is wired to MFP GPIP1. */
#define MFP_PIN_KBD         1

/* ---------------------------------------------------------------- */
/* ST M48T59 TIMEKEEPER: clock + 8 KiB battery-backed NVRAM          */
/*                                                                    */
/* Reference: STMicroelectronics M48T59 datasheet.                    */
/*                                                                    */
/* The whole part is one 8 KiB SRAM window.  The last eight bytes are */
/* the clock and the eight below them are the M48T59's alarm and      */
/* watchdog; everything under 0x1ff0 is ordinary non-volatile RAM.    */
/* Byte accesses only, and every time field is BCD.                   */
/* ---------------------------------------------------------------- */
#define RTC_BASE        0xff600000UL
#define RTC_NVRAM_SIZE  0x1ff0          /* usable NVRAM, 8176 bytes   */

#define RTC_FLAGS       (RTC_BASE + 0x1ff0)  /* R: watchdog/alarm flags */
#define RTC_ALARM_SEC   (RTC_BASE + 0x1ff2)
#define RTC_ALARM_MIN   (RTC_BASE + 0x1ff3)
#define RTC_ALARM_HOUR  (RTC_BASE + 0x1ff4)
#define RTC_ALARM_DATE  (RTC_BASE + 0x1ff5)
#define RTC_INTERRUPTS  (RTC_BASE + 0x1ff6)
#define RTC_WATCHDOG    (RTC_BASE + 0x1ff7)
#define RTC_CONTROL     (RTC_BASE + 0x1ff8)
#define RTC_SECONDS     (RTC_BASE + 0x1ff9)
#define RTC_MINUTES     (RTC_BASE + 0x1ffa)
#define RTC_HOURS       (RTC_BASE + 0x1ffb)
#define RTC_WEEKDAY     (RTC_BASE + 0x1ffc)
#define RTC_DATE        (RTC_BASE + 0x1ffd)
#define RTC_MONTH       (RTC_BASE + 0x1ffe)
#define RTC_YEAR        (RTC_BASE + 0x1fff)

#define RTC_CTL_W       0x80   /* set to write the clock registers    */
#define RTC_CTL_R       0x40   /* set to freeze them for reading      */
#define RTC_SEC_ST      0x80   /* stop bit, in the seconds register   */

#define RTC_FLAG_WDF    0x80   /* watchdog fired                      */
#define RTC_FLAG_AF     0x40   /* alarm fired                         */

#define RTC_INT_ABE     0x20   /* alarm in battery-back-up mode       */
#define RTC_INT_AFE     0x80   /* alarm interrupt enable              */

/* The M48T59 stores two BCD year digits; the board supplies the rest. */
#define RTC_BASE_YEAR   2000

/* The alarm/watchdog output is wired to MFP GPIP2. */
#define MFP_PIN_RTC     2

/* ---------------------------------------------------------------- */
/* Console helpers (uart.c) -- test support library, see above       */
/* ---------------------------------------------------------------- */
#ifndef SAGE040_NO_TESTLIB
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex8(u8 v);
void uart_puthex16(u16 v);
void uart_puthex32(u32 v);
void uart_putdec(u32 v);
int  uart_rx_ready(void);
u8   uart_getc(void);

/* Test result reporting - keeps every test's output uniform. */
void test_begin(const char *name);
void test_ok(const char *what);
void test_fail(const char *what);
void test_end(void);
int  test_failures(void);

/* Halt the machine (STOP with interrupts masked). */
void halt(void) __attribute__((noreturn));
#endif /* SAGE040_NO_TESTLIB */

#endif /* SAGE040_H */
