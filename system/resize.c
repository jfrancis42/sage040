/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * resize - ask the terminal how big it is, and tell the kernel.
 *
 * A terminal on the end of a serial line has no way to say it has been
 * resized, and nothing on this side can measure it. What it can do is
 * answer a question: move the cursor to row 999, column 999 -- which a
 * VT100 clamps to its bottom-right corner -- and ask where the cursor
 * is. The answer is the size. xterm's resize(1) does exactly this.
 *
 * The cursor is saved first and restored afterwards, so running it
 * leaves the screen as it was. The answer is handed to TIOCSWINSZ,
 * which is the size of the LINE: see stty.c for how that combines with
 * the screen's own.
 */
#include "ulib.h"

static int read_reply(char *buf, int max)
{
    struct pollfd p;
    int n = 0;

    p.fd = 0;
    p.events = POLLIN;
    while (n < max - 1 && poll(&p, 1, 2000) > 0) {
        if (read(0, buf + n, 1) != 1) {
            break;
        }
        if (buf[n++] == 'R') {
            break;
        }
    }
    buf[n] = '\0';
    return n;
}

/* ESC [ rows ; cols R */
static int parse(const char *s, int *rows, int *cols)
{
    int r = 0, c = 0;

    while (*s && *s != '[') {
        s++;                    /* skip anything typed ahead of it */
    }
    if (*s++ != '[') {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        r = r * 10 + (*s++ - '0');
    }
    if (*s++ != ';') {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        c = c * 10 + (*s++ - '0');
    }
    if (*s != 'R' || r <= 0 || c <= 0 || r > 255 || c > 255) {
        return -1;
    }
    *rows = r;
    *cols = c;
    return 0;
}

int main(void)
{
    struct termios old, raw;
    struct winsize w;
    char buf[32];
    int rows, cols;

    if (tcgetattr(0, &old) < 0) {
        eputs("resize: standard input is not a terminal\n");
        return 1;
    }
    raw = old;
    raw.c_lflag &= ~(u32)(ICANON | ECHO);
    tcsetattr(0, TCSAFLUSH, &raw);

    puts("\0337\033[999;999H\033[6n");
    read_reply(buf, sizeof(buf));
    puts("\0338");

    tcsetattr(0, TCSANOW, &old);

    if (parse(buf, &rows, &cols) < 0) {
        eputs("resize: the terminal did not say\n");
        return 1;
    }

    memset(&w, 0, sizeof(w));
    w.ws_row = (u16)rows;
    w.ws_col = (u16)cols;
    if (ioctl(0, TIOCSWINSZ, (u32)&w) < 0) {
        eputs("resize: cannot set the size\n");
        return 1;
    }
    puts("resize: ");
    putdec((u32)rows);
    puts(" rows, ");
    putdec((u32)cols);
    puts(" columns\n");
    return 0;
}
