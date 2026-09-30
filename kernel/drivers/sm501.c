/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sm501.c - Silicon Motion SM501 framebuffer.
 *
 * Reference: Silicon Motion SM501 datasheet.
 *
 * Registers itself as the framebuffer "fb0", which fb.c then puts behind
 * /dev/fb0. Nothing above knows this chip exists.
 *
 * Two rules about this part, and getting either wrong produces silence
 * rather than an error:
 *
 *   - Its control registers are LITTLE-endian and accept 32-bit
 *     accesses only. It is a PC-era chip on a big-endian bus, so every
 *     register goes through SM501_WR/SM501_RD, which swap.
 *   - Its video memory is ordinary RAM and reads back in the CPU's own
 *     order. Pixels are written with plain stores, no swapping.
 *
 * Double buffered, because a wireframe drawn a line at a time in front
 * of the viewer flickers badly. A frame is drawn into the buffer that is
 * not on screen and flip() swaps which one the display controller is
 * pointed at -- a single register write, so the change happens between
 * one frame and the next rather than halfway down one.
 *
 * clear(), filled rect() and line() go to the 2D engine; point() alone
 * is the CPU's. The engine's Line Draw is Bresenham in hardware, clipped
 * to the screen by its clip rectangle. (QEMU's model of this chip had
 * no line draw; qemu-patch adds it, as the databook describes it.)
 */
#include "dev.h"
#include "errno.h"
#include "drivers.h"

#define FB0_OFFSET   0x000000UL
#define FB1_OFFSET   0x100000UL

#define DEF_W        640
#define DEF_H        480
#define DEF_BPP      8

/*
 * How long to wait for the blitter before giving up.
 *
 * QEMU performs a fill synchronously, so the status bit is clear by the
 * time it can be read and this never spins. Real silicon does take time.
 * The bound is here because this runs in the kernel: a status bit that
 * never clears, on a chip that is not quite the one expected, would
 * otherwise be a machine that has to be power-cycled.
 */
#define BLIT_TIMEOUT 1000000

static u32 back = FB1_OFFSET;   /* the buffer being drawn into */
static u32 front = FB0_OFFSET;
static int double_buffered = 1;

static int sm501_sync(struct fbdev *f)
{
    u32 spin;

    (void)f;
    for (spin = 0; spin < BLIT_TIMEOUT; spin++) {
        if (!(SM501_RD(SM501_2D_STATUS) & 1)) {
            return 0;
        }
    }
    return -EIO;
}

/* One Rectangle Fill. Seven register writes and the chip does the work. */
static int blit_fill(struct fbdev *f, u32 x, u32 y, u32 w, u32 h,
                     u32 colour)
{
    SM501_WR(SM501_2D_DST_BASE, back);
    SM501_WR(SM501_2D_DEST, (x << 16) | y);
    SM501_WR(SM501_2D_DIMENSION, (w << 16) | h);
    SM501_WR(SM501_2D_PITCH, (f->width << 16) | f->width);
    SM501_WR(SM501_2D_FOREGROUND, colour);
    SM501_WR(SM501_2D_STRETCH, SM501_2D_FMT_8BPP);  /* XY addressing */
    /* ROP2 copy-pen: the fill colour, whatever was there. The ROP field
     * is honoured by the chip for every command; a zero there is ROP3
     * BLACKNESS, which QEMU's fill ignores and real silicon would not. */
    SM501_WR(SM501_2D_CONTROL, SM501_2D_START | SM501_2D_CMD_RECTFILL |
                               SM501_2D_ROP2 | SM501_2D_ROP2_COPYPEN);

    /*
     * Wait before returning. The caller's next act is very likely a CPU
     * write into the buffer the engine is still filling, and on hardware
     * where the fill really does take time that write would be erased.
     */
    return sm501_sync(f);
}

static int sm501_clear(struct fbdev *f, u32 colour)
{
    return blit_fill(f, 0, 0, f->width, f->height, colour);
}

static int sm501_point(struct fbdev *f, int x, int y, u32 colour)
{
    if (x < 0 || y < 0 || (u32)x >= f->width || (u32)y >= f->height) {
        return 0;
    }
    MMIO8(SM501_VRAM + back + (u32)y * f->pitch + (u32)x) = (u8)colour;
    return 0;
}

/*
 * A line, drawn by the 2D engine's Line Draw command.
 *
 * The engine is given the Bresenham terms, not the end point: K1 and K2
 * in the Source register, the start in Destination, the length (the
 * major axis's extent plus one, the end point counting) and the initial
 * error term in Dimension, and the octant as three bits of Control. The
 * error term is one less when x runs forward, which is what makes the
 * line the same pixels whichever end it is drawn from.
 *
 * Clipping is the engine's too: the clip rectangle is the screen, so a
 * line may start or run off it. Its coordinates are 12-bit counters that
 * wrap, which is what makes a start at x = -5 work -- it is 4091, which
 * the clip rectangle excludes, and the counter walks on round to 0 --
 * but only while the wrapped part stays clear of the screen. Hence the
 * limit: every coordinate in [-2048, 2047].
 */
#define LINE_MIN     (-2048)
#define LINE_MAX     2047

static int sm501_line(struct fbdev *f, int x0, int y0, int x1, int y1,
                      u32 colour)
{
    int dx = x1 - x0, dy = y1 - y0, dmaj, dmin;
    u32 ctl = SM501_2D_START | SM501_2D_CMD_LINE | SM501_2D_LAST_PIXEL |
              SM501_2D_ROP2 | SM501_2D_ROP2_COPYPEN;
    int et;

    if (x0 < LINE_MIN || x0 > LINE_MAX || y0 < LINE_MIN || y0 > LINE_MAX ||
        x1 < LINE_MIN || x1 > LINE_MAX || y1 < LINE_MIN || y1 > LINE_MAX) {
        return -EINVAL;
    }
    if (dx < 0) {
        dx = -dx;
        ctl |= SM501_2D_STEP_X_NEG;
    }
    if (dy < 0) {
        dy = -dy;
        ctl |= SM501_2D_STEP_Y_NEG;
    }
    if (dy > dx) {
        ctl |= SM501_2D_MAJOR_Y;
        dmaj = dy;
        dmin = dx;
    } else {
        dmaj = dx;
        dmin = dy;
    }
    et = 2 * dmin - dmaj - (x0 <= x1 ? 1 : 0);

    SM501_WR(SM501_2D_DST_BASE, back);
    SM501_WR(SM501_2D_PITCH, (f->width << 16) | f->width);
    SM501_WR(SM501_2D_STRETCH, SM501_2D_FMT_8BPP);  /* XY addressing */
    SM501_WR(SM501_2D_CLIP_TL, SM501_2D_CLIP_ENABLE);           /* 0, 0 */
    SM501_WR(SM501_2D_CLIP_BR, (f->height << 16) | f->width);  /* exclusive */
    SM501_WR(SM501_2D_FOREGROUND, colour);
    SM501_WR(SM501_2D_SOURCE, ((u32)(2 * dmin) << 16) |
                              ((u32)(2 * (dmin - dmaj)) & 0x3fff));
    SM501_WR(SM501_2D_DEST, (((u32)x0 & 0xfff) << 16) | ((u32)y0 & 0xfff));
    SM501_WR(SM501_2D_DIMENSION, ((u32)(dmaj + 1) << 16) | ((u32)et & 0x3fff));
    SM501_WR(SM501_2D_CONTROL, ctl);

    /* As for a fill: the caller may write the buffer next. */
    return sm501_sync(f);
}

static int sm501_rect(struct fbdev *f, int x, int y, int w, int h,
                      u32 colour, int filled)
{
    if (w <= 0 || h <= 0) {
        return -EINVAL;
    }
    if (!filled) {
        /* Four lines. The engine has no outline mode. */
        sm501_line(f, x, y, x + w - 1, y, colour);
        sm501_line(f, x, y + h - 1, x + w - 1, y + h - 1, colour);
        sm501_line(f, x, y, x, y + h - 1, colour);
        sm501_line(f, x + w - 1, y, x + w - 1, y + h - 1, colour);
        return 0;
    }

    /* Clip before handing it to the engine, which does not. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)f->width)  { w = (int)f->width - x; }
    if (y + h > (int)f->height) { h = (int)f->height - y; }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    return blit_fill(f, (u32)x, (u32)y, (u32)w, (u32)h, colour);
}

/*
 * Show what was just drawn.
 *
 * One register write, then the two buffers change roles. The display
 * controller latches the address, so nothing is torn.
 *
 * Single buffered, there is nothing to show -- drawing already went
 * straight to the visible buffer -- so this does nothing rather than
 * failing, and a program that flips either way works under both.
 */
static int sm501_flip(struct fbdev *f)
{
    (void)f;
    if (!double_buffered) {
        return 0;
    }
    SM501_WR(SM501_PANEL_FB_ADDR, back);
    front = back;
    back = (back == FB0_OFFSET) ? FB1_OFFSET : FB0_OFFSET;
    return 0;
}

/*
 * Draw straight to what is on screen, or to the hidden buffer.
 *
 * A text console wants the first: it draws one character at a time and
 * each has to appear as it goes, with no frame boundary to flip at.
 * Anything animated wants the second. Switching to single buffering
 * points drawing at whatever is currently visible, so the text console
 * does not have to know which of the two that is.
 */
/* Where drawing goes, and where the panel is showing: one and the same
 * when double buffering is off. */
static void sm501_offsets(struct fbdev *f, u32 *draw, u32 *show)
{
    (void)f;
    *draw = back;
    *show = front;
}

static int sm501_setdouble(struct fbdev *f, int on)
{
    (void)f;
    double_buffered = on ? 1 : 0;
    if (!double_buffered) {
        back = front;
    } else if (back == front) {
        back = (front == FB0_OFFSET) ? FB1_OFFSET : FB0_OFFSET;
    }
    return 0;
}

/*
 * Move a rectangle, with the blitter.
 *
 * Scrolling a console is one of these, and doing it with the CPU would
 * be 300 KB of reads and writes per line. ROP 0xcc is a plain source
 * copy; bit 15 of the control word clear selects the three-operand ROP
 * set, which is the one 0xcc belongs to.
 *
 * Moving a region up or left is a forward copy, safe when source and
 * destination overlap. Moving it down or right -- inserting a line or a
 * character on the console -- would overwrite the source before reading
 * it, so those set the right-to-left bit (27), under which the chip
 * starts at the bottom-right corner and the coordinates it is given ARE
 * that corner, not the top-left.
 */
static int sm501_copy(struct fbdev *f, int sx, int sy, int dx, int dy,
                      int w, int h)
{
    u32 ctl = SM501_2D_START | SM501_2D_CMD_BITBLT | 0xccUL;

    if (w <= 0 || h <= 0) {
        return -EINVAL;
    }
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
        (u32)(sx + w) > f->width || (u32)(sy + h) > f->height ||
        (u32)(dx + w) > f->width || (u32)(dy + h) > f->height) {
        return -EINVAL;
    }
    if (dy > sy || (dy == sy && dx > sx)) {
        ctl |= SM501_2D_RTL;
        sx += w - 1;
        sy += h - 1;
        dx += w - 1;
        dy += h - 1;
    }

    SM501_WR(SM501_2D_SRC_BASE, back);
    SM501_WR(SM501_2D_DST_BASE, back);
    SM501_WR(SM501_2D_SOURCE, ((u32)sx << 16) | (u32)sy);
    SM501_WR(SM501_2D_DEST, ((u32)dx << 16) | (u32)dy);
    SM501_WR(SM501_2D_DIMENSION, ((u32)w << 16) | (u32)h);
    SM501_WR(SM501_2D_PITCH, (f->width << 16) | f->width);
    SM501_WR(SM501_2D_STRETCH, SM501_2D_FMT_8BPP);
    SM501_WR(SM501_2D_CONTROL, ctl);

    return sm501_sync(f);
}

/* 0x00RRGGBB in, and the chip takes it in that order. */
static int sm501_palette(struct fbdev *f, u32 index, u32 rgb)
{
    if (index > 255) {
        return -EINVAL;
    }
    (void)f;
    SM501_WR(SM501_PANEL_PALETTE + index * 4, rgb & 0x00ffffffUL);
    return 0;
}

static int sm501_setmode(struct fbdev *f, u32 w, u32 h, u32 bpp)
{
    /*
     * One mode. The panel timing registers below are written for an
     * arbitrary size, but the two buffers are a megabyte apart and the
     * rest of the driver assumes 8 bits per pixel, so anything else is
     * refused rather than half-applied.
     */
    if (bpp != 8 || w == 0 || h == 0 || w > 1024 || h > 1024) {
        return -EINVAL;
    }
    if (w * h > FB1_OFFSET) {
        return -EINVAL;         /* would run into the second buffer */
    }

    f->width = w;
    f->height = h;
    f->bpp = 8;
    f->pitch = w;

    SM501_WR(SM501_PANEL_FB_ADDR, FB0_OFFSET);
    SM501_WR(SM501_PANEL_FB_OFFSET, w);
    SM501_WR(SM501_PANEL_FB_WIDTH, (w << 16) | w);
    SM501_WR(SM501_PANEL_FB_HEIGHT, (h << 16) | h);
    SM501_WR(SM501_PANEL_TL_LOC, 0);
    SM501_WR(SM501_PANEL_BR_LOC, ((w - 1) << 16) | (h - 1));
    SM501_WR(SM501_PANEL_H_TOTAL, w - 1);
    SM501_WR(SM501_PANEL_V_TOTAL, h - 1);
    SM501_WR(SM501_PANEL_CONTROL,
             SM501_PC_ENABLE | SM501_PC_8BPP |
             SM501_PC_FPEN | SM501_PC_VDD | SM501_PC_DATA);

    front = FB0_OFFSET;
    back = FB1_OFFSET;

    /* Both buffers, so nothing inherits whatever was in video memory
     * when the machine started. */
    sm501_clear(f, 0);
    sm501_flip(f);
    sm501_clear(f, 0);
    return 0;
}

static struct fbdev sm501_fb = {
    "fb0",
    DEF_W, DEF_H, DEF_BPP, DEF_W,
    sm501_setmode,
    sm501_point,
    sm501_clear,
    sm501_line,
    sm501_rect,
    sm501_flip,
    sm501_copy,
    sm501_setdouble,
    sm501_sync,
    sm501_palette,
    SM501_VRAM, SM501_VRAM_SIZE,        /* for mmap of /dev/fb0 */
    sm501_offsets,
    0,
    0
};

int sm501_init(void)
{
    /* Is the chip fitted? An address with nothing behind it raises a
     * bus error rather than reading back zeroes, so this has to be
     * asked before the first register access, not by making one. */
    if (!io_probe32((volatile void *)SM501_DEVICEID)) {
        return -ENODEV;
    }

    if (SM501_RD(SM501_DEVICEID) != SM501_DEVICEID_VALUE) {
        return -ENODEV;
    }

    if (sm501_setmode(&sm501_fb, DEF_W, DEF_H, DEF_BPP) < 0) {
        return -EIO;
    }

    /*
     * A default palette: black, then a handful of colours a program can
     * use without setting any up. Index 1 is green because that is what
     * a machine of this era with one colour would have had.
     */
    sm501_palette(&sm501_fb, 0, 0x00000000UL);      /* black   */
    sm501_palette(&sm501_fb, 1, 0x0040ff40UL);      /* green   */
    sm501_palette(&sm501_fb, 2, 0x00ffffffUL);      /* white   */
    sm501_palette(&sm501_fb, 3, 0x00ff4040UL);      /* red     */
    sm501_palette(&sm501_fb, 4, 0x004040ffUL);      /* blue    */
    sm501_palette(&sm501_fb, 5, 0x00ffff40UL);      /* yellow  */
    sm501_palette(&sm501_fb, 6, 0x0040ffffUL);      /* cyan    */
    sm501_palette(&sm501_fb, 7, 0x00ff40ffUL);      /* magenta */

    return dev_register_fb(&sm501_fb);
}
