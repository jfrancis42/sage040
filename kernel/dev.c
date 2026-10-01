/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * dev.c - the device registries.
 *
 * Four singly-linked lists and the functions to add to them and search
 * them. That is the whole thing, and it is worth saying why it is not
 * more: with a handful of soldered-down parts there is nothing to
 * enumerate, no hotplug, no reference counting and nothing to unregister
 * -- a driver that has registered stays registered until the power goes
 * off. Anything cleverer would be machinery in search of a problem.
 *
 * Registration happens once during startup, from main.c, in an order
 * main.c chooses. Nothing here is safe against being called from an
 * interrupt handler, and nothing needs to be.
 */
#include "dev.h"
#include "errno.h"
#include "string.h"

static struct chardev  *chars;
static struct blockdev *blocks;
static struct netdev   *nets;
static struct rtcdev   *the_rtc;
static struct timerdev *the_timer;
static struct fbdev    *fbs;

/* ---------------------------------------------------------------- */
/* Character devices                                                 */
/* ---------------------------------------------------------------- */

/*
 * A device's number, Linux's where Linux has the device. Not for show:
 * programs compare st_rdev and st_ino to tell devices apart, and every
 * device here used to be inode 1, number 0.
 */
static const struct {
    const char *name;
    u16 major, minor;
} linux_numbers[] = {
    { "null", 1, 3 }, { "zero", 1, 5 }, { "full", 1, 7 },
    { "random", 1, 8 }, { "urandom", 1, 9 }, { "klog", 1, 11 },
    { "tty1", 4, 1 }, { "ttyS0", 4, 64 }, { "console", 5, 1 }, { "tty", 5, 0 },
    { "ptmx", 5, 2 }, { "vcsa", 7, 128 }, { "nvram", 10, 144 },
    { "fb0", 29, 0 },
};
#define LOCAL_MAJOR     240     /* Linux's "for local use" range */

static u32 dev_number(const char *name)
{
    static u32 next_local;
    u32 i, n = 0;

    for (i = 0; i < sizeof(linux_numbers) / sizeof(linux_numbers[0]); i++) {
        if (strcmp(name, linux_numbers[i].name) == 0) {
            return ST_DEV(linux_numbers[i].major, linux_numbers[i].minor);
        }
    }
    /* pts/N: the Unix 98 pty slaves are major 136, minor N. */
    if (strncmp(name, "pts/", 4) == 0 && name[4]) {
        for (i = 4; name[i] >= '0' && name[i] <= '9'; i++) {
            n = n * 10 + (u32)(name[i] - '0');
        }
        if (!name[i] && n < 256) {
            return ST_DEV(136, n);
        }
    }
    return ST_DEV(LOCAL_MAJOR, next_local++ & 0xff);
}

int dev_register_char(struct chardev *d)
{
    static u32 next_ino = DEV_INO_FIRST;

    if (!d || !d->name || !d->ops) {
        return -EINVAL;
    }
    if (dev_find_char(d->name)) {
        return -EEXIST;
    }
    if (d->mode == 0) {
        d->mode = DEV_MODE_DEFAULT;
    }
    d->mode &= 07777;
    /* A pty slave registered again gets a new inode, as a new node
     * would on Linux's devpts; its number is its own whatever. */
    d->ino = next_ino++;
    d->rdev = dev_number(d->name);
    d->next = chars;
    chars = d;
    return 0;
}

/*
 * Take a device out of the registry. Only a device that comes and goes
 * needs this -- a pseudo-terminal's slave, which exists between the
 * open of /dev/ptmx and the close of the last descriptor on either end.
 * Hardware is registered once and stays.
 */
int dev_unregister_char(struct chardev *d)
{
    struct chardev **pp;

    for (pp = &chars; *pp; pp = &(*pp)->next) {
        if (*pp == d) {
            *pp = d->next;
            d->next = 0;
            return 0;
        }
    }
    return -ENOENT;
}

/*
 * The other direction: which device is this open file?
 *
 * `who` has to name the terminal a session is on, and the only handle
 * it has is the session leader's descriptor 0. A struct file carries
 * ops and priv but no back-pointer to the chardev it came from, so the
 * registry is walked and matched on both: ops alone is not enough,
 * because every pseudo-terminal slave shares one set of ops and is
 * told apart only by priv.
 *
 * Returns 0 for a file that is not a character device -- a session
 * whose input is a pipe or a file has no terminal, and `who` says so
 * rather than inventing one.
 */
struct chardev *dev_char_for(const struct file *f)
{
    struct chardev *d;

    if (!f || !f->ops) {
        return 0;
    }
    /* BOTH ops AND priv. Every pseudo-terminal slave has its own ops
     * table and its own priv, but the masters share one ops -- so
     * matching on ops alone finds the wrong pty, and matching on priv
     * alone could collide with a device that keeps none. */
    for (d = chars; d; d = d->next) {
        if (d->ops == f->ops && d->priv == f->priv) {
            return d;
        }
    }
    return 0;
}

const char *dev_char_name(const struct file *f)
{
    struct chardev *d = dev_char_for(f);

    return d ? d->name : 0;
}

struct chardev *dev_find_char(const char *name)
{
    struct chardev *d;

    for (d = chars; d; d = d->next) {
        if (strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return 0;
}

struct chardev *dev_first_char(void)
{
    return chars;
}

/* ---------------------------------------------------------------- */
/* Block devices                                                     */
/* ---------------------------------------------------------------- */

int dev_register_block(struct blockdev *b)
{
    if (!b || !b->name || !b->read) {
        return -EINVAL;
    }
    if (dev_find_block(b->name)) {
        return -EEXIST;
    }
    b->next = blocks;
    blocks = b;
    return 0;
}

struct blockdev *dev_find_block(const char *name)
{
    struct blockdev *b;

    for (b = blocks; b; b = b->next) {
        if (strcmp(b->name, name) == 0) {
            return b;
        }
    }
    return 0;
}

struct blockdev *dev_first_block(void)
{
    return blocks;
}

/* ---------------------------------------------------------------- */
/* Network devices                                                   */
/* ---------------------------------------------------------------- */

int dev_register_net(struct netdev *n)
{
    if (!n || !n->name || !n->send || !n->recv) {
        return -EINVAL;
    }
    if (dev_find_net(n->name)) {
        return -EEXIST;
    }
    n->next = nets;
    nets = n;
    return 0;
}

struct netdev *dev_find_net(const char *name)
{
    struct netdev *n;

    for (n = nets; n; n = n->next) {
        if (strcmp(n->name, name) == 0) {
            return n;
        }
    }
    return 0;
}

struct netdev *dev_first_net(void)
{
    return nets;
}

/* ---------------------------------------------------------------- */
/* The clock                                                         */
/* ---------------------------------------------------------------- */

/*
 * One clock, not a list. A second one would raise the question of which
 * is authoritative, and there is no good answer to that question.
 */
int dev_register_rtc(struct rtcdev *r)
{
    if (!r || !r->name || !r->get) {
        return -EINVAL;
    }
    if (the_rtc) {
        return -EEXIST;
    }
    the_rtc = r;
    return 0;
}

struct rtcdev *dev_rtc(void)
{
    return the_rtc;
}

/* ---------------------------------------------------------------- */
/* The timer                                                         */
/* ---------------------------------------------------------------- */

/*
 * One, for the same reason there is one clock: two would raise the
 * question of which one time is measured by.
 */
int dev_register_timer(struct timerdev *t)
{
    if (!t || !t->name || !t->start) {
        return -EINVAL;
    }
    if (the_timer) {
        return -EEXIST;
    }
    the_timer = t;
    return 0;
}

struct timerdev *dev_timer(void)
{
    return the_timer;
}

/* ---------------------------------------------------------------- */
/* Framebuffers                                                      */
/* ---------------------------------------------------------------- */

int dev_register_fb(struct fbdev *f)
{
    if (!f || !f->name || !f->point) {
        /* point() is the one operation everything else can be built
         * from, so a framebuffer without it is not one. */
        return -EINVAL;
    }
    if (dev_find_fb(f->name)) {
        return -EEXIST;
    }
    f->next = fbs;
    fbs = f;
    return 0;
}

struct fbdev *dev_find_fb(const char *name)
{
    struct fbdev *f;

    for (f = fbs; f; f = f->next) {
        if (strcmp(f->name, name) == 0) {
            return f;
        }
    }
    return 0;
}

struct fbdev *dev_first_fb(void)
{
    return fbs;
}

/* ---------------------------------------------------------------- */
/* Stopping the machine                                              */
/* ---------------------------------------------------------------- */

static int (*reset_fn)(void);
static int (*poweroff_fn)(void);

/*
 * First one wins, for both. There is only ever one way to reset a given
 * board and one way to turn it off, and a second registration would
 * mean two drivers each think they own it -- worth keeping the first
 * rather than silently preferring whichever happened to start last.
 */
void dev_register_reset(int (*fn)(void))
{
    if (!reset_fn) {
        reset_fn = fn;
    }
}

void dev_register_poweroff(int (*fn)(void))
{
    if (!poweroff_fn) {
        poweroff_fn = fn;
    }
}

int dev_reset(void)
{
    if (!reset_fn) {
        return -ENODEV;
    }
    return reset_fn();
}

int dev_poweroff(void)
{
    if (!poweroff_fn) {
        return -ENODEV;
    }
    return poweroff_fn();
}
