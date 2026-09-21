/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fbcon.c - a text console on the framebuffer.
 *
 * 80 columns by 30 rows of the IBM PC 8x16 font, green on black --
 * 640x480 divided by the character cell, which is exactly the geometry
 * a VGA text mode had, for exactly the same reason.
 *
 * Registered as /dev/fbcon, and `console fb` in the shell points the
 * kernel's messages and the standard descriptors at it.
 *
 * IT CANNOT READ. This machine has no keyboard -- input arrives on the
 * serial line and nowhere else -- so fbcon_read() hands the call
 * straight to the serial terminal. That is not a workaround for
 * something missing here; it is what the machine is. Output on the
 * screen, input from the wire, and a program cannot tell the difference
 * because both arrive through the same descriptor.
 *
 * Everything it draws goes through `struct fbdev`, so it does not know
 * an SM501 is underneath and would work over anything that can set a
 * pixel.
 */
#include "fbcon.h"
#include "font.h"
#include "dev.h"
#include "vfs.h"
#include "console.h"
#include "errno.h"
#include "string.h"

#define MAX_COLS  128
#define MAX_ROWS  64

#define COL_FG    1             /* green, in the driver's palette */
#define COL_BG    0

#define TABSTOP   8

static struct fbdev *fb;
static struct chardev fbcon_dev;
static struct chardev *serial;  /* where input comes from */

static int cols, rows;
static int cur_col, cur_row;
static int cursor_drawn;
static int mirror;              /* also write to the serial console */

/*
 * What is on the screen.
 *
 * Kept so the console can redraw itself: on a framebuffer with no
 * blitter, scrolling is "shift this and draw it all again", and there is
 * nothing to read pixels back with.
 */
static u8 cells[MAX_ROWS][MAX_COLS];

/* ---------------------------------------------------------------- */
/* Drawing                                                           */
/* ---------------------------------------------------------------- */

/*
 * One character cell.
 *
 * Every pixel of the cell is written, set or clear, so a glyph replaces
 * whatever was there without needing the cell cleared first -- one pass
 * instead of a fill and a draw.
 */
static void draw_cell(int col, int row, u8 ch)
{
    int x0 = col * FONT_WIDTH;
    int y0 = row * FONT_HEIGHT;
    int y, x;

    for (y = 0; y < FONT_HEIGHT; y++) {
        u8 bits = font8x16[ch][y];

        for (x = 0; x < FONT_WIDTH; x++) {
            fb->point(fb, x0 + x, y0 + y,
                      (bits & (0x80 >> x)) ? COL_FG : COL_BG);
        }
    }
}

/* A block on the bottom two rows of the cell, the way a text mode did
 * it. Not blinking: that would want a timer callback, and a steady
 * cursor is no harder to find. */
static void draw_cursor(void)
{
    int x0 = cur_col * FONT_WIDTH;
    int y0 = cur_row * FONT_HEIGHT + FONT_HEIGHT - 2;
    int y, x;

    for (y = 0; y < 2; y++) {
        for (x = 0; x < FONT_WIDTH; x++) {
            fb->point(fb, x0 + x, y0 + y, COL_FG);
        }
    }
    cursor_drawn = 1;
}

static void erase_cursor(void)
{
    if (cursor_drawn) {
        draw_cell(cur_col, cur_row, cells[cur_row][cur_col]);
        cursor_drawn = 0;
    }
}

static void redraw_all(void)
{
    int r, c;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            draw_cell(c, r, cells[r][c]);
        }
    }
}

/*
 * Scroll up one line.
 *
 * With a blitter this is one operation and the console stays usable at
 * any speed. Without one there is no way to read pixels back through
 * `struct fbdev`, so the whole screen is drawn again from `cells` --
 * correct, and slow enough that a driver is worth writing a copy() for.
 */
static void scroll(void)
{
    int c;

    memmove(&cells[0][0], &cells[1][0],
            (u32)(rows - 1) * MAX_COLS);
    for (c = 0; c < cols; c++) {
        cells[rows - 1][c] = ' ';
    }

    if (fb->copy &&
        fb->copy(fb, 0, FONT_HEIGHT, 0, 0,
                 cols * FONT_WIDTH, (rows - 1) * FONT_HEIGHT) == 0) {
        if (fb->rect) {
            fb->rect(fb, 0, (rows - 1) * FONT_HEIGHT,
                     cols * FONT_WIDTH, FONT_HEIGHT, COL_BG, 1);
        } else {
            for (c = 0; c < cols; c++) {
                draw_cell(c, rows - 1, ' ');
            }
        }
        return;
    }

    redraw_all();
}

/* ---------------------------------------------------------------- */
/* Putting characters                                                */
/* ---------------------------------------------------------------- */

static void newline(void)
{
    cur_col = 0;
    if (++cur_row >= rows) {
        cur_row = rows - 1;
        scroll();
    }
}

static void fbcon_putc(u8 ch)
{
    switch (ch) {
    case '\n':
        newline();
        return;

    case '\r':
        cur_col = 0;
        return;

    case '\b':
        if (cur_col > 0) {
            cur_col--;
        }
        return;

    case '\t':
        do {
            cells[cur_row][cur_col] = ' ';
            draw_cell(cur_col, cur_row, ' ');
            if (++cur_col >= cols) {
                newline();
                return;
            }
        } while (cur_col % TABSTOP);
        return;

    case 0x07:                  /* bell: nothing to ring */
        return;

    default:
        break;
    }

    if (ch < 32) {
        return;                 /* other controls are not glyphs */
    }

    cells[cur_row][cur_col] = ch;
    draw_cell(cur_col, cur_row, ch);
    if (++cur_col >= cols) {
        newline();
    }
}

void fbcon_clear(void)
{
    int r, c;

    if (!fb) {
        return;
    }
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            cells[r][c] = ' ';
        }
    }
    if (fb->clear) {
        fb->clear(fb, COL_BG);
    } else {
        redraw_all();
    }
    cur_col = 0;
    cur_row = 0;
    cursor_drawn = 0;
    draw_cursor();
}

/* ---------------------------------------------------------------- */
/* The device                                                        */
/* ---------------------------------------------------------------- */

static s32 fbcon_write(struct file *f, const void *buf, u32 len)
{
    const u8 *p = buf;
    u32 i;

    if (!fb) {
        return -ENODEV;
    }

    /*
     * The double buffering is switched off here rather than once at
     * startup, because a program that drew an animation will have
     * switched it back on and then exited. Whatever put the framebuffer
     * in that state is gone by the time the next character arrives.
     */
    if (fb->setdouble) {
        fb->setdouble(fb, 0);
    }

    erase_cursor();
    for (i = 0; i < len; i++) {
        fbcon_putc(p[i]);
    }
    draw_cursor();

    if (mirror && serial && serial->ops->write) {
        struct file sf;

        memset(&sf, 0, sizeof(sf));
        sf.ops = serial->ops;
        sf.priv = serial->priv;
        sf.used = 1;
        serial->ops->write(&sf, buf, len);
    }

    (void)f;
    return (s32)len;
}

/*
 * Reading the screen means reading the keyboard, and there is not one.
 * The call goes to the serial terminal, which is where every character
 * this machine has ever been typed at came from.
 */
static s32 fbcon_read(struct file *f, void *buf, u32 len)
{
    if (!serial || !serial->ops->read) {
        return -ENODEV;
    }
    return serial->ops->read(f, buf, len);
}

static int fbcon_ioctl(struct file *f, u32 request, u32 arg)
{
    /* Whatever the terminal answers, since that is what input is. */
    if (serial && serial->ops->ioctl) {
        return serial->ops->ioctl(f, request, arg);
    }
    return -ENOTTY;
}

static int fbcon_close(struct file *f)
{
    (void)f;
    return 0;
}

static const struct file_ops fbcon_ops = {
    fbcon_read,
    fbcon_write,
    0,                          /* not seekable */
    fbcon_ioctl,
    fbcon_close
};

int fbcon_mirror(int on)
{
    mirror = on ? 1 : 0;
    return 0;
}

int fbcon_rows(void)
{
    return rows;
}

int fbcon_cols(void)
{
    return cols;
}

int fbcon_init(void)
{
    int err;

    fb = dev_first_fb();
    if (!fb) {
        return -ENODEV;
    }

    cols = (int)(fb->width / FONT_WIDTH);
    rows = (int)(fb->height / FONT_HEIGHT);
    if (cols <= 0 || rows <= 0) {
        return -EINVAL;
    }
    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }
    if (rows > MAX_ROWS) {
        rows = MAX_ROWS;
    }

    /* Input comes from the serial terminal, so there has to be one. */
    serial = dev_find_char("console");
    if (!serial) {
        return -ENODEV;
    }

    if (fb->setdouble) {
        fb->setdouble(fb, 0);
    }

    fbcon_dev.name = "fbcon";
    fbcon_dev.ops = &fbcon_ops;
    fbcon_dev.priv = fb;
    fbcon_dev.next = 0;

    err = dev_register_char(&fbcon_dev);
    if (err < 0) {
        return err;
    }

    fbcon_clear();
    return 0;
}

struct chardev *fbcon_device(void)
{
    return fb ? &fbcon_dev : 0;
}
