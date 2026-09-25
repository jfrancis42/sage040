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

/*
 * The console's sixteen colours, at palette entries 16 to 31 so that
 * the first sixteen stay whatever the driver made them for programs
 * that draw. 0-7 are the ANSI colours, 8-15 their bright versions,
 * which is what bold selects. Green is the default foreground and is
 * the same green the console has always been.
 */
#define PAL_BASE  16

static const u32 ansi_rgb[16] = {
    0x000000UL, 0xd04040UL, 0x40ff40UL, 0xd0d040UL,
    0x4060ffUL, 0xd040d0UL, 0x40d0d0UL, 0xc0c0c0UL,
    0x606060UL, 0xff8080UL, 0xb0ffb0UL, 0xffff80UL,
    0x8090ffUL, 0xff80ffUL, 0x80ffffUL, 0xffffffUL,
};

/*
 * A cell's attribute byte, which is also what /dev/vcsa reports:
 *
 *   bits 0-2  foreground colour     bit 3  bold (the bright foreground)
 *   bits 4-6  background colour     bit 7  underline
 *
 * Reverse video is not stored. It is applied when the byte is made, by
 * swapping the two colours, which is how Linux's console does it too:
 * a reversed cell and a cell written in the swapped colours look the
 * same, so there is no reason to remember which one it was.
 */
#define A_FG(a)       ((a) & 7)
#define A_BOLD        0x08
#define A_BG(a)       (((a) >> 4) & 7)
#define A_UNDER       0x80
#define ATTR_DEFAULT  0x02      /* green on black */

static struct fbdev *fb;
static struct chardev fbcon_dev;
static int has_palette;

static int cols, rows;
static int cur_col, cur_row;
static int cursor_drawn;

/*
 * What is on the screen.
 *
 * Kept so the console can redraw itself: on a framebuffer with no
 * blitter, scrolling is "shift this and draw it all again", and there is
 * nothing to read pixels back with. /dev/vcsa reads it too.
 */
static u8 cells[MAX_ROWS][MAX_COLS];
static u8 attrs[MAX_ROWS][MAX_COLS];

/* ---------------------------------------------------------------- */
/* Terminal state                                                    */
/* ---------------------------------------------------------------- */

/* Graphic renditions as last set by SGR, before reverse is applied. */
static int sgr_fg, sgr_bg, sgr_bold, sgr_under, sgr_reverse;
static u8 cur_attr = ATTR_DEFAULT;

static int top, bot;            /* the scrolling region, inclusive  */
static int wrap_pending;        /* see put_glyph()                  */
static int mode_insert;         /* IRM                              */
static int mode_autowrap = 1;   /* DECAWM                           */
static int mode_origin;         /* DECOM                            */
static int mode_newline;        /* LNM: LF also returns             */
static int cursor_visible = 1;  /* DECTCEM                          */

/* G0 and G1, each either US ASCII ('B') or DEC graphics ('0'). */
static char charset[2] = { 'B', 'B' };
static int shift_out;           /* SO selects G1, SI G0 */

static u8 tabs[MAX_COLS];

/* What ESC 7 saves and ESC 8 restores. */
static struct {
    int col, row, origin, wrap;
    int fg, bg, bold, under, reverse;
    char charset[2];
    int shift_out;
} saved;

/* ---------------------------------------------------------------- */
/* Drawing                                                           */
/* ---------------------------------------------------------------- */

/*
 * A colour number 0-15 to what the driver takes. Without a palette the
 * screen has two colours, and anything but black is the foreground --
 * which keeps reverse video working, since a reversed cell's
 * foreground is black.
 */
static u32 colour(int c)
{
    if (has_palette) {
        return (u32)(PAL_BASE + c);
    }
    return (c & 7) ? COL_FG : COL_BG;
}

static u32 bg_default(void)
{
    return colour(A_BG(ATTR_DEFAULT));
}

/*
 * One character cell.
 *
 * Every pixel of the cell is written, set or clear, so a glyph replaces
 * whatever was there without needing the cell cleared first -- one pass
 * instead of a fill and a draw.
 */
static void draw_cell(int col, int row)
{
    u8 ch = cells[row][col];
    u8 a = attrs[row][col];
    u32 fg = colour(A_FG(a) + ((a & A_BOLD) ? 8 : 0));
    u32 bg = colour(A_BG(a));
    int x0 = col * FONT_WIDTH;
    int y0 = row * FONT_HEIGHT;
    int y, x;

    for (y = 0; y < FONT_HEIGHT; y++) {
        u8 bits = font8x16[ch][y];

        if ((a & A_UNDER) && y == FONT_HEIGHT - 2) {
            bits = 0xff;
        }
        for (x = 0; x < FONT_WIDTH; x++) {
            fb->point(fb, x0 + x, y0 + y, (bits & (0x80 >> x)) ? fg : bg);
        }
    }
}

/* A block on the bottom two rows of the cell, the way a text mode did
 * it. Not blinking: that would want a timer callback, and a steady
 * cursor is no harder to find. */
static void draw_cursor(void)
{
    int x0, y0, y, x;
    u32 c;

    if (!cursor_visible) {
        return;
    }
    x0 = cur_col * FONT_WIDTH;
    y0 = cur_row * FONT_HEIGHT + FONT_HEIGHT - 2;
    c = colour(A_FG(ATTR_DEFAULT) + 8);
    for (y = 0; y < 2; y++) {
        for (x = 0; x < FONT_WIDTH; x++) {
            fb->point(fb, x0 + x, y0 + y, c);
        }
    }
    cursor_drawn = 1;
}

static void erase_cursor(void)
{
    if (cursor_drawn) {
        draw_cell(cur_col, cur_row);
        cursor_drawn = 0;
    }
}

static void redraw_span(int row, int c0, int c1)
{
    int c;

    for (c = c0; c <= c1; c++) {
        draw_cell(c, row);
    }
}

static void redraw_rows(int r0, int r1)
{
    int r;

    for (r = r0; r <= r1; r++) {
        redraw_span(r, 0, cols - 1);
    }
}

/*
 * Blank a span of one row, in the cells and on the screen. Erasing
 * uses the default rendition, not the current one: that is what a
 * VT102 does, and what the vt102 terminfo entry (which has no `bce`)
 * tells programs to expect.
 */
static void blank_span(int row, int c0, int c1)
{
    int c;

    if (c0 > c1) {
        return;
    }
    for (c = c0; c <= c1; c++) {
        cells[row][c] = ' ';
        attrs[row][c] = ATTR_DEFAULT;
    }
    if (fb->rect &&
        fb->rect(fb, c0 * FONT_WIDTH, row * FONT_HEIGHT,
                 (c1 - c0 + 1) * FONT_WIDTH, FONT_HEIGHT,
                 bg_default(), 1) == 0) {
        return;
    }
    redraw_span(row, c0, c1);
}

static void blank_rows(int r0, int r1)
{
    int r, c;

    if (r0 > r1) {
        return;
    }
    for (r = r0; r <= r1; r++) {
        for (c = 0; c < cols; c++) {
            cells[r][c] = ' ';
            attrs[r][c] = ATTR_DEFAULT;
        }
    }
    if (fb->rect &&
        fb->rect(fb, 0, r0 * FONT_HEIGHT, cols * FONT_WIDTH,
                 (r1 - r0 + 1) * FONT_HEIGHT, bg_default(), 1) == 0) {
        return;
    }
    redraw_rows(r0, r1);
}

/*
 * Move a rectangle of cells on the screen with the blitter. Returns 1
 * if it was done; 0 means the caller redraws from `cells` instead,
 * which is always correct and on a framebuffer with no copy() the only
 * way -- there is nothing to read pixels back with.
 */
static int blit(int sc, int sr, int dc, int dr, int w, int h)
{
    if (!fb->copy) {
        return 0;
    }
    return fb->copy(fb, sc * FONT_WIDTH, sr * FONT_HEIGHT,
                    dc * FONT_WIDTH, dr * FONT_HEIGHT,
                    w * FONT_WIDTH, h * FONT_HEIGHT) == 0;
}

static void move_row(int from, int to)
{
    memcpy(cells[to], cells[from], (u32)cols);
    memcpy(attrs[to], attrs[from], (u32)cols);
}

/*
 * Scroll rows r0..r1 up by n, blanking the n at the bottom. Scrolling
 * the whole screen is the case with r0 0 and r1 the last row; a
 * scrolling region and delete-line are the others, and it is the same
 * operation for all three.
 */
static void scroll_up(int r0, int r1, int n)
{
    int moved, r;

    if (n > r1 - r0 + 1) {
        n = r1 - r0 + 1;
    }
    if (n <= 0) {
        return;
    }
    moved = r1 - r0 + 1 - n;
    for (r = 0; r < moved; r++) {
        move_row(r0 + n + r, r0 + r);
    }
    if (moved > 0 && !blit(0, r0 + n, 0, r0, cols, moved)) {
        redraw_rows(r0, r0 + moved - 1);
    }
    blank_rows(r1 - n + 1, r1);
}

/* The other direction: reverse index and insert-line. */
static void scroll_down(int r0, int r1, int n)
{
    int moved, r;

    if (n > r1 - r0 + 1) {
        n = r1 - r0 + 1;
    }
    if (n <= 0) {
        return;
    }
    moved = r1 - r0 + 1 - n;
    for (r = moved - 1; r >= 0; r--) {
        move_row(r0 + r, r0 + n + r);
    }
    if (moved > 0 && !blit(0, r0, 0, r0 + n, cols, moved)) {
        redraw_rows(r0 + n, r1);
    }
    blank_rows(r0, r0 + n - 1);
}

/* ICH: open n blank cells at the cursor, pushing the rest right. */
static void insert_chars(int n)
{
    int row = cur_row, moved;

    if (n > cols - cur_col) {
        n = cols - cur_col;
    }
    if (n <= 0) {
        return;
    }
    moved = cols - cur_col - n;
    if (moved > 0) {
        memmove(&cells[row][cur_col + n], &cells[row][cur_col], (u32)moved);
        memmove(&attrs[row][cur_col + n], &attrs[row][cur_col], (u32)moved);
        if (!blit(cur_col, row, cur_col + n, row, moved, 1)) {
            redraw_span(row, cur_col + n, cols - 1);
        }
    }
    blank_span(row, cur_col, cur_col + n - 1);
}

/* DCH: delete n cells at the cursor, pulling the rest left. */
static void delete_chars(int n)
{
    int row = cur_row, moved;

    if (n > cols - cur_col) {
        n = cols - cur_col;
    }
    if (n <= 0) {
        return;
    }
    moved = cols - cur_col - n;
    if (moved > 0) {
        memmove(&cells[row][cur_col], &cells[row][cur_col + n], (u32)moved);
        memmove(&attrs[row][cur_col], &attrs[row][cur_col + n], (u32)moved);
        if (!blit(cur_col + n, row, cur_col, row, moved, 1)) {
            redraw_span(row, cur_col, cur_col + moved - 1);
        }
    }
    blank_span(row, cols - n, cols - 1);
}

static void erase_all(void)
{
    int r, c;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            cells[r][c] = ' ';
            attrs[r][c] = ATTR_DEFAULT;
        }
    }
    if (fb->clear) {
        fb->clear(fb, bg_default());
    } else {
        redraw_rows(0, rows - 1);
    }
    cursor_drawn = 0;
}

/* ---------------------------------------------------------------- */
/* Replies                                                           */
/* ---------------------------------------------------------------- */

/*
 * A terminal answers some questions -- where is the cursor, what are
 * you -- by typing the answer. Here that means an input source for
 * tty.c, holding whatever the console last said.
 *
 * ONLY WHEN THE SCREEN IS THE ONLY OUTPUT. With the serial line enabled
 * too, the question also went down the wire to a real terminal, which
 * will answer it, and two answers is worse than one: a program reading
 * a cursor position report gets its own reply followed by garbage.
 */
#define REPLY_MAX 32

static struct chardev reply_dev;
static char reply_buf[REPLY_MAX];
static int reply_head, reply_tail;

static int screen_is_only_sink(void)
{
    /*
     * Always, now. The screen is its own terminal (tty1) and the serial
     * line is another (console); nothing is mirrored between them, so
     * the screen is the sole output of its own terminal by construction.
     * It therefore answers DSR/DA itself, and the old worry -- two
     * replies, the screen's and the serial host's, corrupting a
     * program's input -- cannot arise: the serial host answers its own
     * terminal, never this one.
     */
    return 1;
}

static void reply(const char *s)
{
    if (!screen_is_only_sink()) {
        return;
    }
    while (*s) {
        int next = (reply_head + 1) % REPLY_MAX;

        if (next == reply_tail) {
            return;             /* nobody is reading them; drop */
        }
        reply_buf[reply_head] = *s++;
        reply_head = next;
    }
}

static char *put_dec(char *p, int n)
{
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i) {
        *p++ = tmp[--i];
    }
    return p;
}

static s32 reply_read(struct file *f, void *buf, u32 len)
{
    u8 *p = buf;
    u32 n = 0;

    (void)f;
    while (n < len && reply_tail != reply_head) {
        p[n++] = (u8)reply_buf[reply_tail];
        reply_tail = (reply_tail + 1) % REPLY_MAX;
    }
    return (s32)n;
}

static int reply_ioctl(struct file *f, u32 request, u32 arg)
{
    (void)f;
    if (request == FIONREAD) {
        *(u32 *)arg = (u32)((reply_head - reply_tail + REPLY_MAX) % REPLY_MAX);
        return 0;
    }
    return -ENOTTY;
}

static const struct file_ops reply_ops = {
    reply_read, 0, 0, reply_ioctl, 0, 0, 0,
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

/* ---------------------------------------------------------------- */
/* Cursor movement                                                   */
/* ---------------------------------------------------------------- */

/*
 * Every movement goes through here, because every movement cancels a
 * pending wrap -- the one piece of state a VT100 carries that is easy
 * to forget.
 */
static void go(int row, int col)
{
    int lo = 0, hi = rows - 1;

    if (mode_origin) {
        lo = top;
        hi = bot;
    }
    if (row < lo) row = lo;
    if (row > hi) row = hi;
    if (col < 0) col = 0;
    if (col >= cols) col = cols - 1;
    cur_row = row;
    cur_col = col;
    wrap_pending = 0;
}

/* CUP and friends take a row relative to the region in origin mode. */
static void go_abs(int row, int col)
{
    go(row + (mode_origin ? top : 0), col);
}

/* IND: down a row, scrolling the region at its bottom margin. */
static void index_down(void)
{
    wrap_pending = 0;
    if (cur_row == bot) {
        scroll_up(top, bot, 1);
    } else if (cur_row < rows - 1) {
        cur_row++;
    }
}

/* LF is IND, and a carriage return too in newline mode. */
static void linefeed(void)
{
    index_down();
    if (mode_newline) {
        cur_col = 0;
    }
}

static void reverse_index(void)
{
    wrap_pending = 0;
    if (cur_row == top) {
        scroll_down(top, bot, 1);
    } else if (cur_row > 0) {
        cur_row--;
    }
}

/* CUU and CUD stop at a margin if the cursor started inside the region. */
static void cursor_up(int n)
{
    int lim = (cur_row >= top) ? top : 0;
    int r = cur_row - n;

    cur_row = (r < lim) ? lim : r;
    wrap_pending = 0;
}

static void cursor_down(int n)
{
    int lim = (cur_row <= bot) ? bot : rows - 1;
    int r = cur_row + n;

    cur_row = (r > lim) ? lim : r;
    wrap_pending = 0;
}

static void tab_forward(void)
{
    int c = cur_col;

    while (c < cols - 1) {
        c++;
        if (tabs[c]) {
            break;
        }
    }
    cur_col = c;
    wrap_pending = 0;
}

static void tabs_reset(void)
{
    int c;

    for (c = 0; c < MAX_COLS; c++) {
        tabs[c] = (c % 8 == 0 && c != 0);
    }
}

/* ---------------------------------------------------------------- */
/* Renditions                                                        */
/* ---------------------------------------------------------------- */

static void make_attr(void)
{
    int fg = sgr_fg, bg = sgr_bg;

    if (sgr_reverse) {
        fg = sgr_bg;
        bg = sgr_fg;
    }
    cur_attr = (u8)((fg & 7) | ((bg & 7) << 4) |
                    (sgr_bold ? A_BOLD : 0) | (sgr_under ? A_UNDER : 0));
}

static void sgr_reset(void)
{
    sgr_fg = A_FG(ATTR_DEFAULT);
    sgr_bg = A_BG(ATTR_DEFAULT);
    sgr_bold = sgr_under = sgr_reverse = 0;
    make_attr();
}

/*
 * SGR. A VT102 knows bold, underline, blink and reverse; the colours
 * are the ANSI set every later terminal added, accepted because
 * programs send them whatever TERM says and ignoring them costs more
 * than honouring them. Blink is accepted and not shown.
 */
static void sgr(const int *p, int n)
{
    int i;

    if (n == 0) {
        sgr_reset();
        return;
    }
    for (i = 0; i < n; i++) {
        int v = p[i];

        if (v == 0) {
            sgr_reset();
        } else if (v == 1) {
            sgr_bold = 1;
        } else if (v == 4) {
            sgr_under = 1;
        } else if (v == 7) {
            sgr_reverse = 1;
        } else if (v == 22) {
            sgr_bold = 0;
        } else if (v == 24) {
            sgr_under = 0;
        } else if (v == 27) {
            sgr_reverse = 0;
        } else if (v >= 30 && v <= 37) {
            sgr_fg = v - 30;
        } else if (v == 39) {
            sgr_fg = A_FG(ATTR_DEFAULT);
        } else if (v >= 40 && v <= 47) {
            sgr_bg = v - 40;
        } else if (v == 49) {
            sgr_bg = A_BG(ATTR_DEFAULT);
        } else if (v >= 90 && v <= 97) {
            sgr_fg = v - 90;
            sgr_bold = 1;
        }
        /* 5 (blink), 8 and the rest: accepted, not shown */
    }
    make_attr();
}

/* ---------------------------------------------------------------- */
/* Putting characters                                                */
/* ---------------------------------------------------------------- */

/*
 * DEC special graphics -- the line-drawing set, selected with ESC ( 0 --
 * mapped onto the IBM PC font, which has every box piece and most of the
 * rest. Covers 0x5f to 0x7e. Where the PC font has nothing close, a
 * small square.
 */
static const u8 dec_graphics[32] = {
    ' ',  0x04, 0xb1, 0xfe, 0xfe, 0xfe, 0xfe, 0xf8,   /* _ ` a-f   */
    0xf1, 0xfe, 0xfe, 0xd9, 0xbf, 0xda, 0xc0, 0xc5,   /* g-n       */
    0xc4, 0xc4, 0xc4, 0xc4, 0xc4, 0xc3, 0xb4, 0xc1,   /* o-v       */
    0xc2, 0xb3, 0xf3, 0xf2, 0xe3, '#',  0x9c, 0xfa,   /* w-~       */
};

/*
 * One printable character.
 *
 * THE DEFERRED WRAP. Writing into the last column leaves the cursor ON
 * that column with a flag set, and the wrap happens only when the next
 * printable character arrives. That is why a program can write the
 * whole bottom line of the screen without it scrolling, and it is what
 * the vt102 terminfo entry's `xn` flag promises. Wrapping immediately,
 * as this console used to, scrolls the screen every time an editor
 * paints its last line.
 */
static void put_glyph(u8 ch)
{
    if (charset[shift_out] == '0' && ch >= 0x5f && ch <= 0x7e) {
        ch = dec_graphics[ch - 0x5f];
    }

    if (wrap_pending) {
        cur_col = 0;
        index_down();
    }
    if (mode_insert) {
        insert_chars(1);
    }

    cells[cur_row][cur_col] = ch;
    attrs[cur_row][cur_col] = cur_attr;
    draw_cell(cur_col, cur_row);

    if (cur_col < cols - 1) {
        cur_col++;
    } else if (mode_autowrap) {
        wrap_pending = 1;
    }
}

static void save_cursor(void)
{
    saved.col = cur_col;
    saved.row = cur_row;
    saved.origin = mode_origin;
    saved.wrap = wrap_pending;
    saved.fg = sgr_fg;
    saved.bg = sgr_bg;
    saved.bold = sgr_bold;
    saved.under = sgr_under;
    saved.reverse = sgr_reverse;
    saved.charset[0] = charset[0];
    saved.charset[1] = charset[1];
    saved.shift_out = shift_out;
}

static void restore_cursor(void)
{
    mode_origin = saved.origin;
    sgr_fg = saved.fg;
    sgr_bg = saved.bg;
    sgr_bold = saved.bold;
    sgr_under = saved.under;
    sgr_reverse = saved.reverse;
    charset[0] = saved.charset[0];
    charset[1] = saved.charset[1];
    shift_out = saved.shift_out;
    make_attr();
    go(saved.row, saved.col);
    wrap_pending = saved.wrap;
}

/* RIS, and the state at startup. */
static void terminal_reset(void)
{
    top = 0;
    bot = rows - 1;
    mode_insert = 0;
    mode_autowrap = 1;
    mode_origin = 0;
    mode_newline = 0;
    cursor_visible = 1;
    charset[0] = charset[1] = 'B';
    shift_out = 0;
    tabs_reset();
    sgr_reset();
    cur_row = cur_col = 0;
    wrap_pending = 0;
    save_cursor();
}

/* ---------------------------------------------------------------- */
/* Escape sequences                                                  */
/* ---------------------------------------------------------------- */

/*
 * A VT102, parsed the way a VT102 parses: a C0 control inside a
 * sequence is acted on at once and the sequence carries on, CAN and SUB
 * abandon it, and ESC starts a new one. The state names are the ones in
 * Paul Williams' DEC-compatible parser description, which is the
 * reference for this.
 *
 * The line editor still moves only with carriage return and backspace,
 * which work everywhere, so it does not depend on any of this.
 */
enum {
    ST_GROUND,
    ST_ESC,                     /* after ESC                         */
    ST_ESC_INTER,               /* ESC then an intermediate: ( ) #   */
    ST_CSI,                     /* ESC [                             */
    ST_CSI_IGNORE,              /* a CSI too malformed to act on     */
    ST_STRING,                  /* inside OSC/APC/DCS/PM/SOS -- swallow  */
    ST_STRING_ESC,              /* saw ESC in a string: ST is ESC \      */
};

#define MAX_PARAMS 16

static int state;
static int params[MAX_PARAMS];
static int nparams;
static int param_started;
static char csi_private;        /* '?' for DEC private modes         */
static char inter;              /* the intermediate, for ESC ( etc.  */

static int param(int i, int dflt)
{
    if (i >= nparams || params[i] == 0) {
        return dflt;
    }
    return params[i];
}

static void set_mode(int p, int on)
{
    if (csi_private == '?') {
        switch (p) {
        case 6:                 /* DECOM */
            mode_origin = on;
            go_abs(0, 0);
            break;
        case 7:                 /* DECAWM */
            mode_autowrap = on;
            if (!on) {
                wrap_pending = 0;
            }
            break;
        case 25:                /* DECTCEM */
            cursor_visible = on;
            break;
        default:
            /* ?1 (cursor keys) is the keyboard's business, and the
             * keyboard already sends what both modes accept; the rest
             * a screen has no use for. */
            break;
        }
        return;
    }
    switch (p) {
    case 4:                     /* IRM */
        mode_insert = on;
        break;
    case 20:                    /* LNM */
        mode_newline = on;
        break;
    default:
        break;
    }
}

static void csi_dispatch(u8 final)
{
    int i, n;
    char buf[24], *p;

    if (csi_private && final != 'h' && final != 'l') {
        return;                 /* DA2 and friends: nothing to say */
    }

    switch (final) {
    case 'A': cursor_up(param(0, 1));                         break;
    case 'B': case 'e': cursor_down(param(0, 1));             break;
    case 'C': case 'a': go(cur_row, cur_col + param(0, 1));   break;
    case 'D': go(cur_row, cur_col - param(0, 1));             break;
    case 'E': cursor_down(param(0, 1)); cur_col = 0;          break;
    case 'F': cursor_up(param(0, 1)); cur_col = 0;            break;
    case 'G': case '`': go(cur_row, param(0, 1) - 1);         break;
    case 'd': go_abs(param(0, 1) - 1, cur_col);               break;
    case 'H': case 'f':
        go_abs(param(0, 1) - 1, param(1, 1) - 1);
        break;

    case 'J':
        switch (param(0, 0)) {
        case 0:
            blank_span(cur_row, cur_col, cols - 1);
            blank_rows(cur_row + 1, rows - 1);
            break;
        case 1:
            blank_rows(0, cur_row - 1);
            blank_span(cur_row, 0, cur_col);
            break;
        case 2:
            erase_all();
            break;
        default:
            break;
        }
        break;

    case 'K':
        switch (param(0, 0)) {
        case 0: blank_span(cur_row, cur_col, cols - 1);       break;
        case 1: blank_span(cur_row, 0, cur_col);              break;
        case 2: blank_span(cur_row, 0, cols - 1);             break;
        default:                                              break;
        }
        break;

    case 'X':
        n = param(0, 1);
        blank_span(cur_row, cur_col,
                   (cur_col + n > cols ? cols : cur_col + n) - 1);
        break;

    case 'L':                   /* IL, only inside the region */
        if (cur_row >= top && cur_row <= bot) {
            scroll_down(cur_row, bot, param(0, 1));
            cur_col = 0;
            wrap_pending = 0;
        }
        break;
    case 'M':                   /* DL */
        if (cur_row >= top && cur_row <= bot) {
            scroll_up(cur_row, bot, param(0, 1));
            cur_col = 0;
            wrap_pending = 0;
        }
        break;
    case '@': wrap_pending = 0; insert_chars(param(0, 1));    break;
    case 'P': wrap_pending = 0; delete_chars(param(0, 1));    break;
    case 'S': scroll_up(top, bot, param(0, 1));               break;
    case 'T': scroll_down(top, bot, param(0, 1));             break;

    case 'r': {                 /* DECSTBM */
        int t = param(0, 1) - 1;
        int b = param(1, rows) - 1;

        if (b >= rows) {
            b = rows - 1;
        }
        if (t < b) {
            top = t;
            bot = b;
            go_abs(0, 0);
        }
        break;
    }

    case 'g':
        if (param(0, 0) == 0) {
            tabs[cur_col] = 0;
        } else if (param(0, 0) == 3) {
            memset(tabs, 0, sizeof(tabs));
        }
        break;

    case 'm':
        sgr(params, nparams);
        break;

    case 'h':
    case 'l':
        for (i = 0; i < nparams || i == 0; i++) {
            set_mode(i < nparams ? params[i] : 0, final == 'h');
        }
        break;

    case 'n':
        if (param(0, 0) == 5) {
            reply("\033[0n");   /* "no malfunction" */
        } else if (param(0, 0) == 6) {
            p = buf;
            *p++ = '\033';
            *p++ = '[';
            p = put_dec(p, cur_row - (mode_origin ? top : 0) + 1);
            *p++ = ';';
            p = put_dec(p, cur_col + 1);
            *p++ = 'R';
            *p = '\0';
            reply(buf);
        }
        break;

    case 'c':
        if (param(0, 0) == 0) {
            reply("\033[?6c");  /* "I am a VT102" */
        }
        break;

    case 's': save_cursor();                                  break;
    case 'u': restore_cursor();                               break;

    default:
        break;                  /* not one this knows */
    }
}

static void esc_dispatch(u8 ch)
{
    switch (ch) {
    case 'D': index_down();                             break;  /* IND */
    case 'E': cur_col = 0; index_down();                break;  /* NEL */
    case 'M': reverse_index();                          break;  /* RI  */
    case 'H': tabs[cur_col] = 1;                        break;  /* HTS */
    case '7': save_cursor();                            break;
    case '8': restore_cursor();                         break;
    case 'c': terminal_reset(); erase_all();            break;  /* RIS */
    case 'Z': reply("\033[?6c");                        break;  /* DECID */
    default:
        break;                  /* = and > (keypad modes) included */
    }
}

static void esc_inter_dispatch(u8 ch)
{
    int r, c;

    if (inter == '(' || inter == ')') {
        charset[inter == ')'] = (ch == '0') ? '0' : 'B';
    } else if (inter == '#' && ch == '8') {
        /* DECALN: fill the screen with E, the alignment pattern. */
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                cells[r][c] = 'E';
                attrs[r][c] = ATTR_DEFAULT;
            }
        }
        redraw_rows(0, rows - 1);
        go(0, 0);
    }
}

/* The C0 controls, which act the same inside a sequence as outside. */
static void control(u8 ch)
{
    switch (ch) {
    case '\n':
    case 0x0b:                  /* VT */
    case 0x0c:                  /* FF */
        linefeed();
        break;
    case '\r':
        cur_col = 0;
        wrap_pending = 0;
        break;
    case '\b':
        if (cur_col > 0) {
            cur_col--;
        }
        wrap_pending = 0;
        break;
    case '\t':
        tab_forward();
        break;
    case 0x0e:                  /* SO */
        shift_out = 1;
        break;
    case 0x0f:                  /* SI */
        shift_out = 0;
        break;
    default:
        break;                  /* BEL: nothing to ring; the rest: nothing */
    }
}

static void fbcon_putc(u8 ch)
{
    /*
     * A string escape -- OSC (ESC ]), APC (ESC _), DCS (ESC P), PM
     * (ESC ^) or SOS (ESC X) -- carries a run of text a terminal is
     * meant to SWALLOW, not print. The common one is the window title
     * a shell sets before every prompt (ESC ] 0 ; user@host:cwd BEL);
     * a terminal that renders it instead spews "0;user@host:~" onto the
     * screen at every prompt. Consume everything until the terminator:
     * BEL, or ST (ESC \). CAN/SUB abort it, as they abort any sequence.
     */
    if (state == ST_STRING) {
        if (ch == 0x07) {                       /* BEL ends an OSC */
            state = ST_GROUND;
        } else if (ch == 0x1b) {                /* ESC: maybe ST (ESC \) */
            state = ST_STRING_ESC;
        } else if (ch == 0x18 || ch == 0x1a) {  /* CAN, SUB */
            state = ST_GROUND;
        }
        return;                                 /* everything else eaten */
    }
    if (state == ST_STRING_ESC) {
        /* The ESC that ends a string. ESC \ is ST; anything else is
         * malformed, and dropping it is the safe reading either way. */
        state = ST_GROUND;
        return;
    }

    if (ch == 0x1b) {
        state = ST_ESC;
        return;
    }
    if (ch == 0x18 || ch == 0x1a) {     /* CAN, SUB */
        state = ST_GROUND;
        return;
    }
    if (ch < 0x20) {
        control(ch);
        return;
    }
    if (ch == 0x7f) {
        return;                 /* DEL is ignored, as a VT102 does */
    }

    switch (state) {
    case ST_GROUND:
        put_glyph(ch);
        return;

    case ST_ESC:
        if (ch == '[') {
            state = ST_CSI;
            nparams = 0;
            param_started = 0;
            csi_private = 0;
            memset(params, 0, sizeof(params));
        } else if (ch == ']' || ch == '_' || ch == 'P' ||
                   ch == '^' || ch == 'X') {
            state = ST_STRING;      /* OSC, APC, DCS, PM, SOS */
        } else if (ch >= 0x20 && ch <= 0x2f) {
            inter = (char)ch;
            state = ST_ESC_INTER;
        } else {
            state = ST_GROUND;
            esc_dispatch(ch);
        }
        return;

    case ST_ESC_INTER:
        state = ST_GROUND;
        esc_inter_dispatch(ch);
        return;

    case ST_CSI:
        if (ch >= '0' && ch <= '9') {
            if (!param_started) {
                param_started = 1;
                if (nparams < MAX_PARAMS) {
                    nparams++;
                }
            }
            if (params[nparams - 1] < 10000) {
                params[nparams - 1] = params[nparams - 1] * 10 + (ch - '0');
            }
        } else if (ch == ';') {
            if (!param_started && nparams < MAX_PARAMS) {
                nparams++;      /* an empty parameter is a default one */
            }
            param_started = 0;
        } else if (ch >= 0x3c && ch <= 0x3f) {
            csi_private = (char)ch;
        } else if (ch >= 0x20 && ch <= 0x2f) {
            state = ST_CSI_IGNORE;  /* intermediates: none this uses */
        } else if (ch >= 0x40 && ch <= 0x7e) {
            state = ST_GROUND;
            csi_dispatch(ch);
        } else {
            state = ST_CSI_IGNORE;
        }
        return;

    case ST_CSI_IGNORE:
    default:
        if (ch >= 0x40 && ch <= 0x7e) {
            state = ST_GROUND;
        }
        return;
    }
}

void fbcon_clear(void)
{
    if (!fb) {
        return;
    }
    erase_all();
    go(0, 0);
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
    (void)f; (void)arg;
    /*
     * One request: draw everything again from the character buffer.
     * It is what a program that drew over the console wants on its way
     * out, and it is how the tests tell a blitter that moved the wrong
     * pixels from one that moved the right ones -- a screenshot before
     * and after must be identical.
     *
     * And TIOCGWINSZ, which is how tty.c learns how big the screen
     * is without knowing there is a screen. No FIONREAD: that would be
     * answering a question about input, which this is not.
     */
    if (request == TIOCGWINSZ) {
        struct winsize *w = (struct winsize *)arg;

        if (!fb) {
            return -ENODEV;
        }
        w->ws_row = (u16)rows;
        w->ws_col = (u16)cols;
        w->ws_xpixel = (u16)(cols * FONT_WIDTH);
        w->ws_ypixel = (u16)(rows * FONT_HEIGHT);
        return 0;
    }
    if (request == FBCON_REDRAW) {
        if (!fb) {
            return -ENODEV;
        }
        erase_cursor();
        redraw_rows(0, rows - 1);
        draw_cursor();
        return 0;
    }
    return -ENOTTY;
}

static int fbcon_close(struct file *f)
{
    (void)f;
    return 0;
}


/*
 * A character device has no size and no meaningful time; what a caller
 * actually wants from this is S_ISCHR, which is how isatty() is built.
 */
static int fbcon_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR;
    st->st_size = 0;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static const struct file_ops fbcon_ops = {
    fbcon_read,
    fbcon_write,
    0,                          /* not seekable */
    fbcon_ioctl,
    fbcon_close,
    fbcon_fstat,
    0,                          /* poll: the default; see dev.h */
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
};

/*
 * /dev/vcsa: the screen's contents, the way Linux's console offers them.
 * Four bytes -- rows, columns, cursor column, cursor row -- then a
 * character and an attribute byte for every cell, row by row. The
 * attribute byte is described at the top of this file.
 *
 * It exists so that a program can check what the terminal emulator
 * did with a sequence, which is otherwise only visible as pixels.
 */
static struct chardev vcsa_dev;

static s32 vcsa_read(struct file *f, void *buf, u32 len)
{
    u8 *p = buf;
    u32 size, n = 0;

    if (!fb) {
        return -ENODEV;
    }
    size = 4 + (u32)(rows * cols * 2);
    while (n < len && f->pos < size) {
        u32 at = f->pos;

        if (at < 4) {
            p[n] = (u8)(at == 0 ? rows : at == 1 ? cols :
                        at == 2 ? cur_col : cur_row);
        } else {
            u32 cell = (at - 4) / 2;
            int r = (int)(cell / (u32)cols), c = (int)(cell % (u32)cols);

            p[n] = ((at - 4) & 1) ? attrs[r][c] : cells[r][c];
        }
        n++;
        f->pos++;
    }
    return (s32)n;
}

static s32 vcsa_write(struct file *f, const void *buf, u32 len)
{
    (void)f; (void)buf; (void)len;
    return -EINVAL;             /* write to /dev/fbcon instead */
}

static const struct file_ops vcsa_ops = {
    vcsa_read,
    vcsa_write,
    0,
    0,
    fbcon_close,
    fbcon_fstat,
    0,
    0,                          /* truncate: nothing to truncate */
    0,                          /* mmap: not memory to map */
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

    if (fb->palette) {
        int i;

        has_palette = 1;
        for (i = 0; i < 16; i++) {
            if (fb->palette(fb, (u32)(PAL_BASE + i), ansi_rgb[i]) < 0) {
                has_palette = 0;
            }
        }
    }
    terminal_reset();

    fbcon_dev.name = "fbcon";
    fbcon_dev.ops = &fbcon_ops;
    fbcon_dev.priv = fb;
    fbcon_dev.next = 0;

    err = dev_register_char(&fbcon_dev);
    if (err < 0) {
        return err;
    }

    vcsa_dev.name = "vcsa";
    vcsa_dev.ops = &vcsa_ops;
    vcsa_dev.priv = fb;
    vcsa_dev.next = 0;
    err = dev_register_char(&vcsa_dev);
    if (err < 0) {
        return err;
    }

    fbcon_clear();

    /* Answers to "where is the cursor" arrive as input. Not registered
     * under /dev: nothing should open it, only tty.c read it. */
    reply_dev.name = "fbcon";
    reply_dev.ops = &reply_ops;
    reply_dev.priv = 0;
    reply_dev.next = 0;
    err = tty_add_source(&reply_dev);
    if (err < 0) {
        return err;
    }

    /* From here on everything written to the console appears here too. */
    return tty_add_sink(&fbcon_dev);
}

struct chardev *fbcon_device(void)
{
    return fb ? &fbcon_dev : 0;
}
