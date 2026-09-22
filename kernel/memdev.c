/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * memdev.c - /dev/null, /dev/zero and /dev/full.
 *
 * The devices every Unix has that are not hardware, as Linux's
 * drivers/char/mem.c has them:
 *
 *   null   reads end at once; writes are accepted and discarded
 *   zero   reads give zero bytes, as many as asked; writes discarded
 *   full   reads give zero bytes; writes fail with ENOSPC
 *
 * There is deliberately no /dev/random or /dev/urandom: the kernel's
 * generator (random.c) is xorshift, fine for getrandom()'s everyday
 * uses and not a cryptographic source, and a device of that name
 * promises one.
 */
#include "dev.h"
#include "memdev.h"
#include "uapi.h"
#include "errno.h"
#include "string.h"

static s32 null_read(struct file *f, void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return 0;
}

static s32 zero_read(struct file *f, void *buf, u32 len)
{
    (void)f;
    memset(buf, 0, len);
    return (s32)len;
}

static s32 sink_write(struct file *f, const void *buf, u32 len)
{
    (void)f; (void)buf;
    return (s32)len;
}

static s32 full_write(struct file *f, const void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -ENOSPC;
}

/* Seeking is allowed and means nothing, as on Linux. */
static s32 mem_lseek(struct file *f, s32 offset, int whence)
{
    (void)f; (void)offset; (void)whence;
    return 0;
}

static int mem_close(struct file *f)
{
    (void)f;
    return 0;
}

static int mem_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR | 0666;
    st->st_size = 0;
    st->st_blocks = 0;
    return 0;
}

#define MEM_OPS(name, rd, wr)                                            \
    static const struct file_ops name = {                                \
        rd, wr, mem_lseek,                                               \
        0,                      /* no ioctl */                           \
        mem_close, mem_fstat,                                            \
        0,                      /* poll: always ready, the default */    \
        0,                      /* truncate: nothing to truncate */      \
        0,                      /* mmap: nothing to map */               \
    }

MEM_OPS(null_ops, null_read, sink_write);
MEM_OPS(zero_ops, zero_read, sink_write);
MEM_OPS(full_ops, zero_read, full_write);

static struct chardev null_dev = { "null", &null_ops, 0, 0 };
static struct chardev zero_dev = { "zero", &zero_ops, 0, 0 };
static struct chardev full_dev = { "full", &full_ops, 0, 0 };

int memdev_init(void)
{
    int err = dev_register_char(&null_dev);

    if (err == 0) {
        err = dev_register_char(&zero_dev);
    }
    if (err == 0) {
        err = dev_register_char(&full_dev);
    }
    return err;
}
