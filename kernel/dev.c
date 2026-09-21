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

int dev_register_char(struct chardev *d)
{
    if (!d || !d->name || !d->ops) {
        return -EINVAL;
    }
    if (dev_find_char(d->name)) {
        return -EEXIST;
    }
    d->next = chars;
    chars = d;
    return 0;
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
