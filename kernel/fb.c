/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fb.c - /dev/fb0, and the drawing a driver did not do itself.
 *
 * Two jobs.
 *
 * The first is to be a device: a program opens /dev/fb0 and draws with
 * ioctls, and never learns which chip is underneath. The alternative --
 * a dozen graphics system calls -- would tie the kernel's ABI to one
 * kind of hardware, which is exactly what the device model exists to
 * avoid.
 *
 * The second is to make `point` the only operation a driver must
 * implement. Everything else has a generic version here, built from
 * point(), and a driver that leaves clear() or line() null gets it.
 * A driver with a blitter implements them and those are used instead.
 * So a new framebuffer is a few lines and works immediately, and gets
 * faster as its driver learns to do more.
 *
 * Clipping is done here, once, rather than in every driver. A driver's
 * point() may still check -- sm501.c does -- but it does not have to,
 * and nothing that reaches one from this file is out of bounds.
 */
#include "fb.h"
#include "dev.h"
#include "vfs.h"
#include "errno.h"
#include "string.h"

static struct fbdev *fb;
static struct chardev fb_dev;

/* ---------------------------------------------------------------- */
/* Generic drawing, used when a driver does not accelerate it        */
/* ---------------------------------------------------------------- */

static int in_bounds(int x, int y)
{
    return x >= 0 && y >= 0 &&
           (u32)x < fb->width && (u32)y < fb->height;
}

static int generic_clear(u32 colour)
{
    u32 x, y;

    for (y = 0; y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) {
            fb->point(fb, (int)x, (int)y, colour);
        }
    }
    return 0;
}

/*
 * Bresenham. Integer only, no division, and it draws the same set of
 * pixels travelling in either direction -- which matters more than it
 * looks: an outline drawn as four lines should not be a pixel thicker
 * on two of its sides.
 */
static int generic_line(int x0, int y0, int x1, int y1, u32 colour)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1;
    int sy = dy < 0 ? -1 : 1;
    int err, e2;
    int guard = 0;

    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    err = dx - dy;

    for (;;) {
        if (in_bounds(x0, y0)) {
            fb->point(fb, x0, y0, colour);
        }
        if (x0 == x1 && y0 == y1) {
            return 0;
        }
        /* A line is at most width+height pixels long. The bound is here
         * because this runs in the kernel on behalf of a program that
         * supplied the coordinates, and a loop that cannot terminate is
         * a machine that has to be power-cycled. */
        if (++guard > (int)(fb->width + fb->height) * 2) {
            return -EINVAL;
        }
        e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static int draw_line(int x0, int y0, int x1, int y1, u32 colour)
{
    if (fb->line) {
        return fb->line(fb, x0, y0, x1, y1, colour);
    }
    return generic_line(x0, y0, x1, y1, colour);
}

static int generic_rect(int x, int y, int w, int h, u32 colour, int filled)
{
    int i;

    if (w <= 0 || h <= 0) {
        return -EINVAL;
    }
    if (!filled) {
        draw_line(x, y, x + w - 1, y, colour);
        draw_line(x, y + h - 1, x + w - 1, y + h - 1, colour);
        draw_line(x, y, x, y + h - 1, colour);
        draw_line(x + w - 1, y, x + w - 1, y + h - 1, colour);
        return 0;
    }
    for (i = 0; i < h; i++) {
        draw_line(x, y + i, x + w - 1, y + i, colour);
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* The ioctls                                                        */
/* ---------------------------------------------------------------- */

static int do_getinfo(struct fb_info *out)
{
    int i;

    if (!out) {
        return -EINVAL;
    }
    out->width = fb->width;
    out->height = fb->height;
    out->bpp = fb->bpp;
    out->pitch = fb->pitch;
    for (i = 0; i < 15 && fb->name[i]; i++) {
        out->name[i] = fb->name[i];
    }
    out->name[i] = '\0';
    return 0;
}

static int fb_ioctl(struct file *f, u32 request, u32 arg)
{
    (void)f;

    if (!fb) {
        return -ENODEV;
    }

    switch (request) {
    case FBIO_GETINFO:
        return do_getinfo((struct fb_info *)arg);

    case FBIO_SETMODE: {
        const struct fb_mode *m = (const struct fb_mode *)arg;

        if (!m) {
            return -EINVAL;
        }
        if (!fb->setmode) {
            return -ENOSYS;
        }
        return fb->setmode(fb, m->width, m->height, m->bpp);
    }

    case FBIO_POINT: {
        const struct fb_point *p = (const struct fb_point *)arg;

        if (!p) {
            return -EINVAL;
        }
        if (!in_bounds(p->x, p->y)) {
            /* Off the screen is not an error -- a program drawing a
             * shape that runs off the edge should not have to clip
             * first. It just does not happen. */
            return 0;
        }
        return fb->point(fb, p->x, p->y, p->colour);
    }

    case FBIO_LINE: {
        const struct fb_line *l = (const struct fb_line *)arg;

        if (!l) {
            return -EINVAL;
        }
        return draw_line(l->x0, l->y0, l->x1, l->y1, l->colour);
    }

    case FBIO_RECT: {
        const struct fb_rect *r = (const struct fb_rect *)arg;

        if (!r) {
            return -EINVAL;
        }
        if (fb->rect) {
            return fb->rect(fb, r->x, r->y, r->w, r->h, r->colour,
                            (int)r->filled);
        }
        return generic_rect(r->x, r->y, r->w, r->h, r->colour,
                            (int)r->filled);
    }

    case FBIO_CLEAR:
        if (fb->clear) {
            return fb->clear(fb, arg);
        }
        return generic_clear(arg);

    case FBIO_FLIP:
        if (!fb->flip) {
            return -ENOSYS;
        }
        return fb->flip(fb);

    case FBIO_SYNC:
        if (!fb->sync) {
            return 0;           /* nothing to wait for */
        }
        return fb->sync(fb);

    case FBIO_COPY: {
        const struct fb_copy *c = (const struct fb_copy *)arg;

        if (!c) {
            return -EINVAL;
        }
        if (!fb->copy) {
            /*
             * No generic version: there is no way to read a pixel back
             * through this interface, so there is nothing to copy from.
             * A caller that needs to move pixels on hardware without a
             * blitter has to redraw them from whatever it drew them out
             * of, which is what the text console does.
             */
            return -ENOSYS;
        }
        return fb->copy(fb, c->sx, c->sy, c->dx, c->dy, c->w, c->h);
    }

    case FBIO_DOUBLE:
        if (!fb->setdouble) {
            return -ENOSYS;
        }
        return fb->setdouble(fb, (int)arg);

    case FBIO_PALETTE: {
        const struct fb_palette *p = (const struct fb_palette *)arg;

        if (!p) {
            return -EINVAL;
        }
        if (!fb->palette) {
            return -ENOSYS;
        }
        return fb->palette(fb, p->index, p->rgb);
    }

    default:
        return -ENOTTY;
    }
}

/*
 * Reading and writing the device is not supported.
 *
 * On Linux you would mmap it and write pixels yourself, which is the
 * right answer and needs an MMU that is not turned on here. Rather than
 * invent a slower substitute nobody asked for, the ioctls do the
 * drawing and read/write say plainly that they are not the way in.
 */
static s32 fb_no_read(struct file *f, void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EINVAL;
}

static s32 fb_no_write(struct file *f, const void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EINVAL;
}

static int fb_close(struct file *f)
{
    (void)f;
    return 0;
}


/*
 * A character device has no size and no meaningful time; what a caller
 * actually wants from this is S_ISCHR, which is how isatty() is built.
 */
static int fb_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR;
    st->st_size = 0;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static const struct file_ops fb_ops = {
    fb_no_read,
    fb_no_write,
    0,                          /* not seekable */
    fb_ioctl,
    fb_close,
    fb_fstat,
    0,                          /* poll: the default; see dev.h */
};

int fb_init(void)
{
    fb = dev_first_fb();
    if (!fb) {
        return -ENODEV;
    }

    fb_dev.name = "fb0";
    fb_dev.ops = &fb_ops;
    fb_dev.priv = fb;
    fb_dev.next = 0;

    return dev_register_char(&fb_dev);
}
