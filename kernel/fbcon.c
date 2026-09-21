/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fbcon.c - a text console on the framebuffer.
 *
 * 80 columns by 30 rows of the IBM PC 8x16 font, green on black --
 * 640x480 divided by the character cell, which is exactly the geometry
 * a VGA text mode had, for exactly the same reason.
 *
 * Registered as /dev/fbcon and handed to the terminal layer as an
 * output sink, so everything written to the console appears here as well
 * as on the serial line.
 *
 * IT ONLY WRITES. A screen is not an input device; on this machine
 * characters arrive on the serial port, and will shortly also arrive
 * from a keyboard. Neither is this file's business -- tty.c collects
 * input from wherever it comes and sends output to wherever it goes,
 * and this is one of the wheres.
 *
 * Everything it draws goes through `struct fbdev`, so it does not know
 * an SM501 is underneath and would work over anything that can set a
 * pixel.
 */
#include "fbcon.h"
#include "font.h"
#include "dev.h"
#include "vfs.h"
#include "tty.h"
#include "errno.h"
#include "string.h"

#define MAX_COLS  128
#define MAX_ROWS  64

#define COL_FG    1             /* green, in the driver's palette */
#define COL_BG    0

#define TABSTOP   8

static struct fbdev *fb;
static struct chardev fbcon_dev;

static int cols, rows;
static int cur_col, cur_row;
static int cursor_drawn;

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

/*
 * Just enough ANSI to be clearable.
 *
 * The serial console on the other end of the wire is a real terminal and
 * understands escape sequences; this one understood none, so anything
 * that wanted to clear the screen had to know which sink it was talking
 * to. Teaching this the two sequences that matter means the shell can
 * write ESC [ H ESC [ 2 J and have both sinks do the right thing, which
 * is the whole point of having sinks.
 *
 * Deliberately not a full terminal emulator. Cursor addressing would
 * invite the line editor to use it, and the editor is carefully built
 * out of carriage return and backspace so that it works here at all.
 */
enum { ESC_NONE, ESC_SAW_ESC, ESC_BRACKET };

static int esc_state;
static int esc_param;

static void fbcon_home(void)
{
    cur_col = 0;
    cur_row = 0;
}

static void fbcon_erase_all(void)
{
    int r, c;

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
    cursor_drawn = 0;
}

/* Returns 1 if the character was consumed by an escape sequence. */
static int fbcon_escape(u8 ch)
{
    switch (esc_state) {
    case ESC_NONE:
        if (ch == 0x1b) {
            esc_state = ESC_SAW_ESC;
            return 1;
        }
        return 0;

    case ESC_SAW_ESC:
        if (ch == '[') {
            esc_state = ESC_BRACKET;
            esc_param = 0;
        } else {
            esc_state = ESC_NONE;   /* not a sequence this knows */
        }
        return 1;

    case ESC_BRACKET:
    default:
        if (ch >= '0' && ch <= '9') {
            esc_param = esc_param * 10 + (ch - '0');
            return 1;
        }
        switch (ch) {
        case 'J':
            /* 2J is the whole screen, which is the only one used. */
            if (esc_param == 2) {
                fbcon_erase_all();
            }
            break;
        case 'H':
            fbcon_home();
            break;
        case 'K': {
            int c;

            for (c = cur_col; c < cols; c++) {
                cells[cur_row][c] = ' ';
                draw_cell(c, cur_row, ' ');
            }
            break;
        }
        default:
            break;                  /* silently ignore the rest */
        }
        esc_state = ESC_NONE;
        return 1;
    }
}

static void fbcon_putc(u8 ch)
{
    if (fbcon_escape(ch)) {
        return;
    }

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

    (void)f;
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

    return (s32)len;
}

/*
 * A screen cannot be read from. Saying so plainly is better than
 * quietly forwarding the call somewhere else: input has a path of its
 * own through tty.c, and a caller that ends up here has the wrong
 * device rather than a device that needs helping along.
 */
static s32 fbcon_read(struct file *f, void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EINVAL;
}

static int fbcon_ioctl(struct file *f, u32 request, u32 arg)
{
    (void)f; (void)request; (void)arg;
    /* Nothing to configure, and nothing to report: FIONREAD on a screen
     * would be answering a question about input, which this is not. */
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

    /* From here on everything written to the console appears here too. */
    return tty_add_sink(&fbcon_dev);
}

struct chardev *fbcon_device(void)
{
    return fb ? &fbcon_dev : 0;
}
