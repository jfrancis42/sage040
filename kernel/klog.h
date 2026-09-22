/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * klog.h - the kernel's log ring. See klog.c.
 *
 * Everything the kernel prints goes here as well as to the console, and
 * `klogd` copies it from /dev/klog into /var/log/syslog. The kernel
 * itself never writes to a file: the first messages exist before there
 * is a filesystem, and a panic has to be able to speak whatever state
 * the disk is in.
 */
#ifndef KLOG_H
#define KLOG_H

#include "kernel.h"

void klog_init(void);           /* registers /dev/klog               */
void klog_putc(char c);         /* from console.c, any context       */
void klog_write(const char *buf, u32 len);
u32  klog_pending(void);        /* bytes a reader has not taken      */
u32  klog_lost(void);           /* bytes overwritten before a read   */

#endif /* KLOG_H */
