/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * vtcheck - the framebuffer console's VT102 emulation.
 *
 *   vtcheck         sequences written to /dev/fbcon, and what they did
 *                   read back from /dev/vcsa: characters, attributes and
 *                   the cursor, cell by cell.
 *   vtcheck blit    fills the screen, then does every operation that
 *                   moves pixels with the blitter, and waits. The
 *                   harness takes a screenshot, presses a key, this asks
 *                   the console to redraw everything from its character
 *                   buffer, and the harness takes another. They must be
 *                   identical -- the redraw is right by construction, so
 *                   a difference is the blitter having moved the wrong
 *                   pixels. /dev/vcsa cannot see that; only pixels can.
 *
 * The console sink is switched off while this runs, so that nothing it
 * prints -- or the shell echoes -- lands on the screen being checked.
 */
#include "ulib.h"

static int fb;
static u8 scr[4 + 128 * 64 * 2];
static int rows, cols;

static void report(const char *what, int ok)
{
    puts(ok ? "  ok   " : "  FAIL ");
    puts(what);
    putch('\n');
}

static void out(const char *s)
{
    write(fb, s, strlen(s));
}

static void snap(void)
{
    int fd = open("/dev/vcsa", O_RDONLY);

    memset(scr, 0, sizeof(scr));
    if (fd >= 0) {
        read(fd, scr, sizeof(scr));
        close(fd);
    }
    rows = scr[0];
    cols = scr[1];
}

static int ch(int r, int c)  { return scr[4 + (r * cols + c) * 2]; }
static int at(int r, int c)  { return scr[5 + (r * cols + c) * 2]; }
static int cx(void)          { return scr[2]; }
static int cy(void)          { return scr[3]; }

static int cursor_is(int r, int c)
{
    return cy() == r && cx() == c;
}

/* Row r reads s, and is blank after it. */
static int row_is(int r, const char *s)
{
    int c, n = (int)strlen(s);

    for (c = 0; c < cols; c++) {
        if (ch(r, c) != (c < n ? (u8)s[c] : ' ')) {
            return 0;
        }
    }
    return 1;
}

static int rows_blank(int r0, int r1)
{
    int r;

    for (r = r0; r <= r1; r++) {
        if (!row_is(r, "")) {
            return 0;
        }
    }
    return 1;
}

/* Put `s` at the start of each of rows r0..r1, as "<s><row number>". */
static void label_rows(int r0, int r1, char tag)
{
    char b[16];
    int r;

    for (r = r0; r <= r1; r++) {
        b[0] = '\033'; b[1] = '[';
        b[2] = (char)('0' + (r + 1) / 10);
        b[3] = (char)('0' + (r + 1) % 10);
        b[4] = ';'; b[5] = '1'; b[6] = 'H';
        b[7] = tag;
        b[8] = (char)('0' + r / 10);
        b[9] = (char)('0' + r % 10);
        b[10] = '\0';
        out(b);
    }
}

static int label_at(int r, char tag, int n)
{
    return ch(r, 0) == (u8)tag && ch(r, 1) == '0' + n / 10 &&
           ch(r, 2) == '0' + n % 10;
}

static void console_sink(int on)
{
    struct console_set cs;

    memset(&cs, 0, sizeof(cs));
    memcpy(cs.name, "fbcon", sizeof("fbcon"));
    cs.on = on;
    ioctl(0, TIOCSCONS, (u32)&cs);
}

static void test_cursor(void)
{
    out("\033c");
    snap();
    report("RIS clears the screen and homes the cursor",
           rows > 0 && cols > 0 && rows_blank(0, rows - 1) &&
           cursor_is(0, 0));

    out("\033[5;10HX");
    snap();
    report("CUP puts a character at row 5, column 10",
           ch(4, 9) == 'X' && cursor_is(4, 10));
    out("\033[H");
    snap();
    report("CUP with no parameters is home", cursor_is(0, 0));
    out("\033[99;999H");
    snap();
    report("CUP past the edges stops at the corner",
           cursor_is(rows - 1, cols - 1));
    out("\033[;5H");
    snap();
    report("an empty parameter is the default", cursor_is(0, 4));

    out("\033[10;10H\033[3A");
    snap();
    report("CUU", cursor_is(6, 9));
    out("\033[2B");
    snap();
    report("CUD", cursor_is(8, 9));
    out("\033[5C");
    snap();
    report("CUF", cursor_is(8, 14));
    out("\033[20D");
    snap();
    report("CUB stops at column 1", cursor_is(8, 0));
    out("\033[12G\033[3d");
    snap();
    report("CHA and VPA", cursor_is(2, 11));

    out("\033[3;3H\0337\033[1m\033[10;10H\0338X");
    snap();
    report("ESC 7 / ESC 8 save and restore the position",
           ch(2, 2) == 'X' && at(2, 2) == 0x02);

    out("\033[5;\r7H");
    snap();
    report("a control inside a sequence is acted on, and the sequence "
           "completes", cursor_is(4, 6));
    out("\033[1;1H\033[5\030Y");
    snap();
    report("CAN abandons a sequence", ch(0, 0) == 'Y' && cursor_is(0, 1));
}

static void test_wrap(void)
{
    out("\033c");
    out("\033[1;80HA");
    snap();
    report("the last column leaves the cursor on it (deferred wrap)",
           ch(0, cols - 1) == 'A' && cursor_is(0, cols - 1) &&
           row_is(1, ""));
    out("B");
    snap();
    report("  and the next character wraps",
           ch(1, 0) == 'B' && cursor_is(1, 1));

    out("\033[1;1HTOP\033[30;80HZ");
    snap();
    report("filling the bottom-right cell does not scroll",
           ch(rows - 1, cols - 1) == 'Z' && ch(0, 0) == 'T');

    out("\033[3;80HA\bB");
    snap();
    report("backspace after a deferred wrap moves from the last column",
           ch(2, cols - 2) == 'B' && ch(2, cols - 1) == 'A');

    out("\033[?7l\033[5;79HABCD");
    snap();
    report("with autowrap off the last column is overwritten",
           ch(4, cols - 2) == 'A' && ch(4, cols - 1) == 'D' &&
           row_is(5, ""));
    out("\033[?7h");

    out("\033c\033[1;1HFIRST\033[30;1H\n");
    snap();
    report("a line feed on the bottom row scrolls the screen",
           row_is(0, "") && cursor_is(rows - 1, 0));
    out("\033c\033[1;1HABC\n");
    snap();
    report("LF is a line feed only: the column is kept",
           cursor_is(1, 3));
    out("\033[20hX\n");
    snap();
    report("  unless newline mode is set", cursor_is(2, 0));
    out("\033[20l");
}

static void test_erase(void)
{
    int c, ok;

    out("\033c");
    label_rows(0, 5, 'L');
    out("\033[2;3H\033[J");
    snap();
    report("ED 0 erases from the cursor to the end",
           label_at(0, 'L', 0) && ch(1, 0) == 'L' && ch(1, 1) == '0' &&
           ch(1, 2) == ' ' && rows_blank(2, rows - 1));

    out("\033c");
    label_rows(0, 5, 'L');
    out("\033[3;2H\033[1J");
    snap();
    report("ED 1 erases from the start to the cursor",
           rows_blank(0, 1) && ch(2, 0) == ' ' && ch(2, 1) == ' ' &&
           ch(2, 2) == '2' && label_at(3, 'L', 3));

    out("\033[2J");
    snap();
    report("ED 2 erases everything, and leaves the cursor",
           rows_blank(0, rows - 1) && cursor_is(2, 1));

    out("\033c\033[1;1HABCDEFGH\033[1;4H\033[K");
    snap();
    report("EL 0", row_is(0, "ABC"));
    out("\033[1;1HABCDEFGH\033[1;4H\033[1K");
    snap();
    report("EL 1", ch(0, 3) == ' ' && ch(0, 4) == 'E' && ch(0, 0) == ' ');
    out("\033[2K");
    snap();
    report("EL 2", row_is(0, ""));
    out("\033[1;1HABCDEFGH\033[1;2H\033[3X");
    snap();
    report("ECH erases characters without moving the rest",
           row_is(0, "A   EFGH") && cursor_is(0, 1));

    out("\033c\033[7m\033[1;1HXX\033[2K\033[J");
    snap();
    ok = 1;
    for (c = 0; c < cols; c++) {
        ok = ok && at(0, c) == 0x02;
    }
    report("erasing uses the normal rendition, not the current one", ok);
    out("\033[0m");
}

static void test_insert_delete(void)
{
    int c, ok;

    out("\033c\033[1;1HABCDEF\033[1;3H\033[2@");
    snap();
    report("ICH opens blanks at the cursor",
           row_is(0, "AB  CDEF") && cursor_is(0, 2));
    out("\033[2P");
    snap();
    report("DCH closes them again", row_is(0, "ABCDEF"));

    out("\033c\033[1;1H");
    for (c = 0; c < cols; c++) {
        char b[2];

        b[0] = (char)('A' + c % 26);
        b[1] = '\0';
        out(b);
    }
    out("\033[1;1H\033[@");
    snap();
    ok = ch(0, 0) == ' ';
    for (c = 1; c < cols; c++) {
        ok = ok && ch(0, c) == 'A' + (c - 1) % 26;
    }
    report("ICH pushes the last character off the end", ok);
    out("\033[1;1H\033[5P");
    snap();
    ok = 1;
    for (c = 0; c < cols - 5; c++) {
        ok = ok && ch(0, c) == 'A' + (c + 4) % 26;
    }
    for (; c < cols; c++) {
        ok = ok && ch(0, c) == ' ';
    }
    report("DCH pulls the rest left and blanks the end", ok);

    out("\033c\033[1;1HABC\033[1;1H\033[4hXY\033[4lZ");
    snap();
    report("insert mode inserts, replace mode replaces",
           row_is(0, "XYZBC"));

    out("\033c");
    label_rows(0, 5, 'R');
    out("\033[3;4H\033[L");
    snap();
    report("IL opens a line and returns to column 1",
           label_at(1, 'R', 1) && row_is(2, "") && label_at(3, 'R', 2) &&
           label_at(6, 'R', 5) && cursor_is(2, 0));
    out("\033[2M");
    snap();
    report("DL closes lines and pulls the rest up",
           label_at(1, 'R', 1) && label_at(2, 'R', 3) &&
           label_at(4, 'R', 5) && rows_blank(5, rows - 1));
}

static void test_region(void)
{
    out("\033c\033[5;10r");
    snap();
    report("DECSTBM homes the cursor", cursor_is(0, 0));

    label_rows(0, 12, 'S');
    out("\033[10;1H\n");
    snap();
    report("LF at the bottom margin scrolls only the region",
           label_at(3, 'S', 3) && label_at(4, 'S', 5) &&
           label_at(8, 'S', 9) && row_is(9, "") &&
           label_at(10, 'S', 10) && cursor_is(9, 0));

    out("\033[5;1H\033M");
    snap();
    report("RI at the top margin scrolls the region down",
           label_at(3, 'S', 3) && row_is(4, "") &&
           label_at(5, 'S', 5) && label_at(10, 'S', 10));

    out("\033[2;1H\033[L");
    snap();
    report("IL outside the region does nothing",
           label_at(1, 'S', 1) && label_at(2, 'S', 2));

    out("\033[7;1H\033[L");
    snap();
    report("IL inside the region pushes lines off its bottom margin",
           row_is(6, "") && label_at(7, 'S', 6) &&
           label_at(10, 'S', 10));

    out("\033[?6h\033[1;1HO");
    snap();
    report("origin mode addresses from the top margin",
           ch(4, 0) == 'O' && cursor_is(4, 1));
    out("\033[99;1H");
    snap();
    report("  and cannot leave the region", cursor_is(9, 0));
    out("\033[?6l\033[r");
    snap();
    report("resetting the region homes the cursor", cursor_is(0, 0));
}

static void test_attrs(void)
{
    out("\033c\033[7mR\033[0mN\033[1mB\033[0;4mU\033[0;31;44mC\033[m"
        "\033[1;7mZ\033[22;27mP\033[0;93mY\033[0m");
    snap();
    report("reverse swaps the colours", at(0, 0) == 0x20);
    report("SGR 0 is green on black", at(0, 1) == 0x02);
    report("bold", at(0, 2) == 0x0a);
    report("underline", at(0, 3) == 0x82);
    report("ANSI colours", at(0, 4) == 0x41);
    report("bold and reverse together", at(0, 5) == 0x28);
    report("22 and 27 turn bold and reverse off", at(0, 6) == 0x02);
    report("bright colours are bold ones", at(0, 7) == 0x0b);

    out("\033c\033(0lqkxmj\033(Bq");
    snap();
    report("DEC graphics draws boxes from the PC font",
           ch(0, 0) == 0xda && ch(0, 1) == 0xc4 && ch(0, 2) == 0xbf &&
           ch(0, 3) == 0xb3 && ch(0, 4) == 0xc0 && ch(0, 5) == 0xd9 &&
           ch(0, 6) == 'q');
    out("\033)0a\016q\017q");
    snap();
    report("  as G1, through shift-out and shift-in",
           ch(0, 7) == 'a' && ch(0, 8) == 0xc4 && ch(0, 9) == 'q');

    out("\033c\tX\033[3g\033[1;6H\033H\r\tY");
    snap();
    report("tab stops: every 8 at reset, cleared, and set",
           ch(0, 8) == 'X' && ch(0, 5) == 'Y');
    out("\033c\033#8");
    snap();
    report("DECALN fills the screen with E",
           ch(0, 0) == 'E' && ch(rows - 1, cols - 1) == 'E' &&
           cursor_is(0, 0));
}

/* Only when the screen is the sole output does the console answer. */
static void test_replies(void)
{
    struct termios old, raw;
    struct pollfd p;
    char b[16];
    int n;

    tcgetattr(0, &old);
    raw = old;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw);

    out("\033c\033[5;10H\033[6n");
    p.fd = 0;
    p.events = POLLIN;
    report("with the serial line enabled there is no reply",
           poll(&p, 1, 300) == 0);

    {
        struct console_set cs;

        memset(&cs, 0, sizeof(cs));
        memcpy(cs.name, "fbcon", sizeof("fbcon"));
        cs.on = 1;
        ioctl(0, TIOCSCONS, (u32)&cs);
        memcpy(cs.name, "ttyS0", sizeof("ttyS0"));
        cs.on = 0;
        ioctl(0, TIOCSCONS, (u32)&cs);

        write(1, "\033[6n", 4);
        n = 0;
        while (n < (int)sizeof(b) - 1 && poll(&p, 1, 500) > 0 &&
               read(0, b + n, 1) == 1) {
            if (b[n++] == 'R') {
                break;
            }
        }
        b[n] = '\0';
        write(1, "\033[c", 3);
        n = 0;
        {
            char d[16];

            while (n < (int)sizeof(d) - 1 && poll(&p, 1, 500) > 0 &&
                   read(0, d + n, 1) == 1) {
                if (d[n++] == 'c') {
                    break;
                }
            }
            d[n] = '\0';

            cs.on = 1;
            ioctl(0, TIOCSCONS, (u32)&cs);      /* serial back */
            memcpy(cs.name, "fbcon", sizeof("fbcon"));
            cs.on = 0;
            ioctl(0, TIOCSCONS, (u32)&cs);

            report("alone, it answers DSR 6 with the cursor position",
                   strcmp(b, "\033[5;10R") == 0);
            report("  and DA with VT102", strcmp(d, "\033[?6c") == 0);
        }
    }
    tcsetattr(0, TCSANOW, &old);
}

static void wait_key(void)
{
    char c;

    read(0, &c, 1);
}

static void blit_pattern(void)
{
    int r;

    out("\033c");
    for (r = 0; r < rows; r++) {
        char b[8];

        b[0] = '\033'; b[1] = '[';
        b[2] = (char)('0' + (r % 7) + 1); b[3] = 'm';
        b[4] = '\0';
        out(b);
        label_rows(r, r, (char)('a' + r % 26));
        out("\033[0m .......... the quick brown fox jumps over the lazy dog");
    }
    out("\033[4;1H\033[2L");            /* IL: moves down (right-to-left) */
    out("\033[12;1H\033[3M");           /* DL: moves up                   */
    out("\033[6;20H\033[7@");           /* ICH: moves right               */
    out("\033[7;10H\033[5P");           /* DCH: moves left                */
    out("\033[15;25r\033[25;1H\n\n");   /* region scrolls up              */
    out("\033[15;1H\033M\033M\033M");   /* region scrolls down            */
    out("\033[r\033[30;1H\n");          /* the whole screen scrolls       */
    out("\033[1;1H\033M");              /* and down                       */
    out("\033[20;30HBLIT-DONE");
}

int main(int argc, char **argv)
{
    fb = open("/dev/fbcon", O_WRONLY);
    if (fb < 0) {
        puts("vtcheck: no /dev/fbcon\n");
        return 1;
    }
    snap();
    if (rows == 0) {
        puts("vtcheck: no /dev/vcsa\n");
        return 1;
    }

    console_sink(0);

    if (argc > 1 && strcmp(argv[1], "blit") == 0) {
        struct termios old, raw;

        tcgetattr(0, &old);
        raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &raw);
        blit_pattern();
        puts("vtcheck: ready for the first screenshot\n");
        wait_key();
        ioctl(fb, FBCON_REDRAW, 0);
        puts("vtcheck: redrawn, ready for the second\n");
        wait_key();
        tcsetattr(0, TCSANOW, &old);
        console_sink(1);
        puts("vtcheck: blit done\n");
        return 0;
    }

    test_cursor();
    test_wrap();
    test_erase();
    test_insert_delete();
    test_region();
    test_attrs();
    test_replies();

    out("\033c");
    console_sink(1);
    puts("vtcheck: done\n");
    return 0;
}
