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
#include "uapi.h"       /* struct stat, for the fstat op below */

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

    /*
     * Describe an OPEN file, which is not the same question as
     * describing a path. A ported program stats descriptors constantly
     * -- to size a file it is about to read, or to find out whether
     * what it has is a terminal -- and it has no path to ask about.
     *
     * Null means "no better answer than the defaults", which the VFS
     * fills in.
     */
    int (*fstat)(struct file *f, struct stat *st);

    /*
     * Ready for what? POLLIN, POLLOUT, POLLERR, POLLHUP, now, without
     * waiting. poll() and select() are built on this and nothing else.
     *
     * Null means: answer from FIONREAD if the ioctl knows it (readable
     * when bytes are waiting, always writable), and otherwise always
     * ready both ways -- which is the truth for a regular file, since
     * reading one never waits.
     */
    int (*poll)(struct file *f);

    /*
     * Make the file exactly `len` bytes: cut off what is past it, or
     * add zeroes up to it. ftruncate() is this. Null for anything that
     * has no length -- a device, a pipe -- which gets EINVAL.
     */
    int (*truncate)(struct file *f, u32 len);

    /*
     * The physical page behind byte `offset` of a DEVICE, for mmap: what
     * lets a program draw into /dev/fb0 as memory. Null for anything
     * that is not memory to map -- a file is mapped by copying (or by
     * sharing its read-only pages, textcache.c), not through this.
     * Returns 0 with *pa, or -errno (EINVAL past the end).
     */
    int (*mmap)(struct file *f, u32 offset, u32 *pa);
};

struct file {
    const struct file_ops *ops;
    void *priv;                 /* whatever the driver needs        */
    u32   pos;                  /* byte offset, for seekable things */
    int   flags;                /* the O_* flags it was opened with */
    int   used;
    int   refs;                 /* how many descriptors point here  */
    int   fs;                   /* the filesystem's: calls take its lock */
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
int  dev_unregister_char(struct chardev *d);
struct chardev *dev_find_char(const char *name);
/* Which registered device an open file is, or 0 if it is not one. */
const char *dev_char_name(const struct file *f);
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
    /*
     * Move a rectangle within the framebuffer. This is what scrolling a
     * text console is, and a chip with a blitter does it in one
     * operation. Optional: fb.c has no generic version, because there is
     * no way to read a pixel back through this interface -- a console on
     * a framebuffer without it redraws from its own character buffer
     * instead, which it has anyway.
     */
    int  (*copy)(struct fbdev *f, int sx, int sy, int dx, int dy,
                 int w, int h);
    /*
     * Double buffering on or off. A console wants it off: it draws a
     * character at a time and each one must appear, with no frame to
     * flip. An animation wants it on. Optional; a driver that does not
     * implement it is always single buffered.
     */
    int  (*setdouble)(struct fbdev *f, int on);
    int  (*sync)(struct fbdev *f);      /* wait for the blitter       */
    int  (*palette)(struct fbdev *f, u32 index, u32 rgb);
    /*
     * For mmap: where the video memory is, and how much of it; and
     * where in it drawing goes and where the display shows (they differ
     * while double buffering is on). Optional: without them /dev/fb0
     * cannot be mapped.
     */
    u32  mem_phys, mem_size;
    void (*offsets)(struct fbdev *f, u32 *draw, u32 *show);
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

/* ---------------------------------------------------------------- */
/* Stopping the machine                                              */
/*                                                                    */
/* Two different things, and this board can do only one of them.      */
/*                                                                    */
/* A RESET restarts the machine. Not a class of device, because it is */
/* not a device: it is one thing that one chip on the board happens   */
/* to be able to do. The keyboard controller can pull the processor's */
/* reset line, which is how every PC since 1984 has rebooted itself   */
/* -- an accident of the IBM PC's design that outlived every other    */
/* part of it.                                                        */
/*                                                                    */
/* A POWER-OFF makes the machine go away, and needs something that    */
/* can cut the supply. Nothing on this board can, so nothing          */
/* registers one and `dev_poweroff` answers -ENODEV. A board with a   */
/* power controller registers that driver and nothing above changes.  */
/*                                                                    */
/* Both are registered rather than called by name so that reboot()    */
/* does not have to know a keyboard is involved, which is the same    */
/* reason everything else here is a structure of function pointers.   */
/* ---------------------------------------------------------------- */

void dev_register_reset(int (*fn)(void));
void dev_register_poweroff(int (*fn)(void));

/* Each returns 0 if something took it -- in which case the machine is
 * on its way out and the call may not return at all -- or -ENODEV if
 * nothing on this board can do that. */
int  dev_reset(void);
int  dev_poweroff(void);
#endif /* DEV_H */
