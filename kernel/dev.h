/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * dev.h - the device model.
 *
 * Everything the kernel talks to is one of six kinds of device, and each
 * kind has exactly one interface that the layers above it use:
 *
 *   struct chardev    a byte stream        -> the console, and the /dev names
 *   struct blockdev   addressable sectors  -> the disk a filesystem sits on
 *   struct netdev     packets              -> an ethernet interface
 *   struct rtcdev     seconds since 1970   -> the battery-backed clock
 *   struct timerdev   a periodic interrupt -> what makes time pass
 *   struct fbdev      a display            -> point, line, rect, flip
 *
 * The point is that nothing above these structures names a chip. The
 * filesystem asks a `struct blockdev` for sector 2048; it does not know
 * whether an ATA taskfile, a SCSI controller or a RAM disk answers. The
 * shell writes to a file descriptor; it does not know whether an
 * NS16550A or some other UART is on the other end. Replacing a driver
 * means writing one of these structures and registering it, and nothing
 * above it changes -- which is the whole reason the indirection is here
 * and worth its cost.
 *
 * Drivers register themselves during startup. There is no probing by
 * bus enumeration, because there is no enumerable bus: this is a board
 * with parts soldered to it, and main.c knows which ones.
 */
#ifndef DEV_H
#define DEV_H

#include "kernel.h"

struct file;

/* ---------------------------------------------------------------- */
/* What you can do to an open file, whatever is behind it            */
/* ---------------------------------------------------------------- */

struct file_ops {
    s32 (*read)(struct file *f, void *buf, u32 len);
    s32 (*write)(struct file *f, const void *buf, u32 len);
    s32 (*lseek)(struct file *f, s32 offset, int whence);
    int (*ioctl)(struct file *f, u32 request, u32 arg);
    int (*close)(struct file *f);
};

struct file {
    const struct file_ops *ops;
    void *priv;                 /* whatever the driver needs        */
    u32   pos;                  /* byte offset, for seekable things */
    int   flags;                /* the O_* flags it was opened with */
    int   used;
};

/* ---------------------------------------------------------------- */
/* Character devices                                                 */
/* ---------------------------------------------------------------- */

struct chardev {
    const char *name;           /* as it appears under /dev         */
    const struct file_ops *ops;
    void *priv;
    struct chardev *next;       /* the registry is a plain list     */
};

int  dev_register_char(struct chardev *d);
struct chardev *dev_find_char(const char *name);
struct chardev *dev_first_char(void);

/* ---------------------------------------------------------------- */
/* Block devices                                                     */
/* ---------------------------------------------------------------- */

struct blockdev {
    const char *name;           /* "hda"                            */
    const char *model;          /* what the drive calls itself      */
    u32  sector_size;
    u32  sectors;               /* capacity                         */
    int  (*read)(struct blockdev *b, u32 lba, u32 count, void *buf);
    int  (*write)(struct blockdev *b, u32 lba, u32 count, const void *buf);
    void *priv;
    struct blockdev *next;
};

int  dev_register_block(struct blockdev *b);
struct blockdev *dev_find_block(const char *name);
struct blockdev *dev_first_block(void);

/* ---------------------------------------------------------------- */
/* Real-time clocks                                                  */
/*                                                                    */
/* One at a time, and the kernel only ever asks it for seconds since  */
/* the epoch.  Every part numbers its registers differently -- BCD,   */
/* binary, two-digit years, index/data ports -- and none of that is   */
/* the filesystem's business.                                         */
/* ---------------------------------------------------------------- */

struct rtcdev {
    const char *name;
    int (*get)(struct rtcdev *r, time_t *out);
    int (*set)(struct rtcdev *r, time_t secs);
    void *priv;
};

int  dev_register_rtc(struct rtcdev *r);
struct rtcdev *dev_rtc(void);

/* ---------------------------------------------------------------- */
/* Periodic timers                                                   */
/*                                                                    */
/* One at a time: the thing that makes time pass. A driver starts its */
/* hardware at the requested rate and calls timer_tick() from its     */
/* interrupt handler; nothing above it knows which chip, which        */
/* channel or which prescaler.                                        */
/* ---------------------------------------------------------------- */

struct timerdev {
    const char *name;
    u32  hz;                    /* what it is actually running at     */
    int  (*start)(struct timerdev *t, u32 hz);
    int  (*stop)(struct timerdev *t);
    void *priv;
};

int  dev_register_timer(struct timerdev *t);
struct timerdev *dev_timer(void);

/* ---------------------------------------------------------------- */
/* Framebuffers                                                      */
/*                                                                    */
/* A display is none of the other classes: not a byte stream, not     */
/* addressable sectors, not packets. It gets its own.                 */
/*                                                                    */
/* Only point() is required. A chip with a blitter implements clear,  */
/* line and rect as well and they are used; one without leaves them   */
/* null and the generic code in fb.c does the same work with point(), */
/* slower but correct. That is the whole reason for the split.        */
/* ---------------------------------------------------------------- */

struct fbdev {
    const char *name;           /* "fb0"                              */
    u32  width;
    u32  height;
    u32  bpp;
    u32  pitch;                 /* bytes per row                      */

    int  (*setmode)(struct fbdev *f, u32 w, u32 h, u32 bpp);
    int  (*point)(struct fbdev *f, int x, int y, u32 colour);
    int  (*clear)(struct fbdev *f, u32 colour);
    int  (*line)(struct fbdev *f, int x0, int y0, int x1, int y1,
                 u32 colour);
    int  (*rect)(struct fbdev *f, int x, int y, int w, int h,
                 u32 colour, int filled);
    int  (*flip)(struct fbdev *f);      /* show what was just drawn   */
    int  (*sync)(struct fbdev *f);      /* wait for the blitter       */
    int  (*palette)(struct fbdev *f, u32 index, u32 rgb);
    void *priv;
    struct fbdev *next;
};

int  dev_register_fb(struct fbdev *f);
struct fbdev *dev_find_fb(const char *name);
struct fbdev *dev_first_fb(void);

/* ---------------------------------------------------------------- */
/* Network devices                                                   */
/* ---------------------------------------------------------------- */

#define NET_MTU      1514       /* a full ethernet frame, no FCS     */
#define NET_ADDR_LEN 6

struct netdev {
    const char *name;           /* "eth0"                           */
    u8   mac[NET_ADDR_LEN];
    int  (*up)(struct netdev *n);
    int  (*down)(struct netdev *n);
    int  (*send)(struct netdev *n, const void *frame, u32 len);
    /* Returns the frame length, 0 if nothing is waiting, or -errno. */
    s32  (*recv)(struct netdev *n, void *frame, u32 max);
    void *priv;
    struct netdev *next;
};

int  dev_register_net(struct netdev *n);
struct netdev *dev_find_net(const char *name);
struct netdev *dev_first_net(void);

#endif /* DEV_H */
