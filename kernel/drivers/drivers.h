/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * drivers.h - the drivers this board has, and how to start them.
 *
 * Each of these probes its chip and, if it is there, registers a device
 * with the model in dev.h. Each returns 0 or a negated errno, and a
 * failure is not fatal: a machine with no disk should still get a
 * console and a prompt, and say why there is no disk.
 *
 * This list is the board. There is no bus to enumerate -- the parts are
 * soldered down -- so main.c calls them in an order it chooses, and
 * swapping a chip means swapping a line here and adding a file.
 */
#ifndef DRIVERS_H
#define DRIVERS_H

/*
 * Is anything at this address?
 *
 * A read that survives a bus error, in memprobe.s. Returns 1 if the
 * access completed and 0 if nothing answered; it never writes and never
 * looks at the value, because an absent chip and a chip holding zero
 * read the same and the question is only whether the bus replied.
 *
 * EVERY DRIVER CALLS ONE OF THESE BEFORE ITS FIRST REGISTER ACCESS.
 * QEMU faults on an address with no device behind it, exactly as a real
 * board faults on an empty socket, so without this a kernel run on an
 * emulator built before one of its devices existed does not report a
 * missing device -- it panics inside the first driver that reaches for
 * one, which reads as a kernel bug and is not. That has happened once
 * already, with the keyboard.
 *
 * Probe at the width the driver will use. Several device regions
 * declare a minimum access size and a too-narrow access lands in the
 * wrong byte lane silently: SM501 registers are 32-bit only, while the
 * MFP, the UART and the 8042 are byte registers.
 */
int io_probe8(volatile void *addr);
int io_probe16(volatile void *addr);
int io_probe32(volatile void *addr);

int mfp_init(void);         /* interrupt controller + the system timer     */
void mfp_interrupts_on(void);
int mfp_request_irq(int channel, void (*handler)(void *), void *arg);
int mfp_request_gpip(int pin, void (*handler)(void *), void *arg);
int ns16550_irq_on(void);
void ata_counts(u32 *slept, u32 *polled);
int ata_irq_on(void);
void ata_set_delay(u32 ms);
void mfp_counts(u32 out[16]);
u32 mfp_spurious(void);

int ns16550_init(void);     /* serial port       -> /dev/ttyS0             */
int ns16550_present(void);
struct chardev *ns16550_device(void);
int ata_init(void);         /* disk              -> block device "hda"     */
int m48t59_init(void);      /* clock and NVRAM   -> the system clock       */
int sm501_init(void);       /* video             -> framebuffer "fb0"      */
int i8042_init(void);       /* keyboard          -> /dev/kbd0, a tty source */
int i8042_present(void);
struct chardev *i8042_device(void);
void smc91c111_init(void);  /* ethernet          -> net device "eth0"      */

int fat16_init(void);       /* not a driver: registers the filesystem type */

#endif /* DRIVERS_H */
