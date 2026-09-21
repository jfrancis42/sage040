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

int mfp_init(void);         /* interrupt controller + the system timer     */
void mfp_interrupts_on(void);
int mfp_request_irq(int channel, void (*handler)(void *), void *arg);
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
