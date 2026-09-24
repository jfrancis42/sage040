/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * klog.c - what the kernel said, kept so that something can read it
 * back.
 *
 * Every character the kernel prints goes to the console AND into the
 * ring here. The console is where a person sees it; the ring is where a
 * program gets it, and the difference matters because the console
 * scrolls, is sometimes nobody, and cannot be read at all once the line
 * has gone past.
 *
 * WHY A RING AND NOT A FILE. The kernel does not write to the
 * filesystem. It cannot: the first messages are printed before there is
 * a disk driver, let alone a mounted volume, and a panic must be able to
 * say so with the filesystem in any state at all. So the kernel keeps
 * the bytes and a PROGRAM copies them out -- `klogd` (system/klogd.c),
 * started from /etc/rc, reading /dev/klog and appending to
 * /var/log/syslog. That is the same division Linux draws, for the same
 * reason.
 *
 * The ring OVERWRITES its oldest bytes when it is full, because the
 * alternative is a kernel that stops being able to report things
 * because nobody read the last report. What is lost is what a reader
 * was too slow for, and the reader is told how much (`klog_lost`).
 */
#include "klog.h"
#include "kernel.h"
#include "dev.h"
#include "vfs.h"
#include "uapi.h"
#include "errno.h"
#include "wait.h"
#include "poll.h"
#include "task.h"
#include "signal.h"
#include "uaccess.h"
#include "string.h"

#define KLOG_SIZE 8192          /* a power of two: the mask below */

static char  ring[KLOG_SIZE];
static u32   head;              /* where the kernel writes           */
static u32   tail;              /* where a reader has got to         */
static u32   lost;              /* bytes overwritten before a read   */
static struct waitq readers;

/*
 * Called from console_write and kputc, with interrupts possibly
 * masked and from any context at all -- including the middle of a
 * panic. It takes no locks and can fail at nothing.
 */
void klog_putc(char c)
{
    ring[head & (KLOG_SIZE - 1)] = c;
    head++;
    if (head - tail > KLOG_SIZE) {
        /* The reader fell behind: the oldest byte has just gone. */
        tail = head - KLOG_SIZE;
        lost++;
    }
}

void klog_write(const char *buf, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++) {
        klog_putc(buf[i]);
    }
    if (readers.head) {
        wake_all(&readers);
    }
}

u32 klog_lost(void)
{
    return lost;
}

u32 klog_pending(void)
{
    return head - tail;
}

/* --- /dev/klog -------------------------------------------------------- */

/*
 * A read DRAINS: what one reader takes, another will not see. That is
 * /proc/kmsg's rule rather than /dev/kmsg's, and it is the right one
 * here because the only reader is klogd, whose whole job is to move the
 * bytes somewhere they can be read repeatedly.
 *
 * A read with nothing to read BLOCKS, so klogd is a task asleep rather
 * than a loop asking. It returns as soon as there is anything at all,
 * like a terminal, rather than waiting for a whole line.
 */
static s32 klog_read(struct file *f, void *buf, u32 len)
{
    char *out = buf;
    u32 n = 0;

    (void)f;
    if (len == 0) {
        return 0;
    }
    while (head == tail) {
        if (f->flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        sleep_on(&readers);
        if (signal_pending(current)) {
            return -EINTR;
        }
    }
    while (n < len && tail != head) {
        out[n++] = ring[tail & (KLOG_SIZE - 1)];
        tail++;
    }
    return (s32)n;
}

/* Writing to it puts a line in the log, which is how a program says
 * something the kernel's own log should carry -- klogd's own "started"
 * line, and syslog(3) if it is ever pointed here. */
static s32 klog_dev_write(struct file *f, const void *buf, u32 len)
{
    (void)f;
    klog_write(buf, len);
    return (s32)len;
}

/* Readable when the kernel has said something a reader has not taken;
 * always writable, since a write is a memcpy into the ring. */
static int klog_poll(struct file *f)
{
    (void)f;
    return POLLOUT | (head != tail ? POLLIN : 0);
}

static int klog_ioctl(struct file *f, u32 req, u32 arg)
{
    (void)f;
    switch (req) {
    case FIONREAD: {
        u32 n = head - tail;

        return copy_to_user(arg, &n, sizeof(n));
    }
    default:
        return -EINVAL;
    }
}

static const struct file_ops klog_ops = {
    klog_read,
    klog_dev_write,
    0,                          /* lseek: a ring has no position     */
    klog_ioctl,
    0,                          /* close                             */
    0,                          /* fstat: the defaults               */
    klog_poll,
    0,                          /* truncate: it has no length        */
    0,                          /* mmap                              */
};

static struct chardev klog_dev = { .name = "klog", .ops = &klog_ops };

void klog_init(void)
{
    dev_register_char(&klog_dev);
}
