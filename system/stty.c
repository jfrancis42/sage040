/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * stty - the terminal's modes and size.
 *
 *   stty                  the size and the flags that are honoured
 *   stty size             "ROWS COLS", which is what scripts parse
 *   stty rows N cols N    set the size of the line (see below)
 *   stty [-]echo [-]icanon [-]isig [-]icrnl [-]onlcr [-]opost
 *   stty raw | -raw | sane
 *
 * THE SIZE SET HERE IS THE SERIAL LINE'S. The screen knows its own size
 * and reports it; the terminal at the other end of the wire cannot be
 * asked except by `resize`, so it is taken to be 24x80 until somebody
 * says otherwise. What a program is told is the smaller of every output
 * the console is writing to, because a full-screen program has to fit on
 * all of them at once. So after `stty rows 40` with the screen enabled
 * too, `stty size` still says 30: the screen has 30.
 */
#include "ulib.h"

static int to_int(const char *s)
{
    int n = 0;

    if (!*s) {
        return -1;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        n = n * 10 + (*s - '0');
    }
    return n;
}

struct flag {
    const char *name;
    int which;                  /* 0 iflag, 1 oflag, 3 lflag */
    u32 bit;
};

static const struct flag flags[] = {
    { "icrnl",  0, ICRNL  },
    { "opost",  1, OPOST  },
    { "onlcr",  1, ONLCR  },
    { "isig",   3, ISIG   },
    { "icanon", 3, ICANON },
    { "echo",   3, ECHO   },
};
#define NFLAGS ((int)(sizeof(flags) / sizeof(flags[0])))

static u32 *field(struct termios *t, int which)
{
    switch (which) {
    case 0:  return &t->c_iflag;
    case 1:  return &t->c_oflag;
    default: return &t->c_lflag;
    }
}

static void show(const struct termios *t, const struct winsize *w)
{
    int i;

    puts("rows ");
    putdec(w->ws_row);
    puts("; columns ");
    putdec(w->ws_col);
    puts(";\n");
    for (i = 0; i < NFLAGS; i++) {
        struct termios c = *t;

        if (!(*field(&c, flags[i].which) & flags[i].bit)) {
            putch('-');
        }
        puts(flags[i].name);
        putch(i == NFLAGS - 1 ? '\n' : ' ');
    }
}

static void usage(void)
{
    eputs("usage: stty [size] [rows N] [cols N] [[-]FLAG...] "
          "[raw|-raw|sane]\n");
}

int main(int argc, char **argv)
{
    struct termios t;
    struct winsize w;
    int i, set_modes = 0, set_size = 0, err;

    if (tcgetattr(0, &t) < 0 || ioctl(0, TIOCGWINSZ, (u32)&w) < 0) {
        eputs("stty: standard input is not a terminal\n");
        return 1;
    }
    if (argc == 1) {
        show(&t, &w);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int neg = (a[0] == '-');
        int f;

        if (strcmp(a, "size") == 0) {
            putdec(w.ws_row);
            putch(' ');
            putdec(w.ws_col);
            putch('\n');
            continue;
        }
        if (strcmp(a, "rows") == 0 || strcmp(a, "cols") == 0 ||
            strcmp(a, "columns") == 0) {
            int n = (i + 1 < argc) ? to_int(argv[i + 1]) : -1;

            if (n <= 0 || n > 255) {
                eputs("stty: ");
                eputs(a);
                eputs(" wants a number from 1 to 255\n");
                return 1;
            }
            if (a[0] == 'r') {
                w.ws_row = (u16)n;
            } else {
                w.ws_col = (u16)n;
            }
            set_size = 1;
            i++;
            continue;
        }
        if (strcmp(a, "raw") == 0) {
            t.c_lflag &= ~(u32)(ICANON | ECHO | ISIG);
            t.c_iflag &= ~(u32)ICRNL;
            t.c_cc[VMIN] = 1;
            t.c_cc[VTIME] = 0;
            set_modes = 1;
            continue;
        }
        if (strcmp(a, "-raw") == 0 || strcmp(a, "sane") == 0) {
            t.c_lflag |= ICANON | ECHO | ISIG;
            t.c_iflag |= ICRNL;
            t.c_oflag |= OPOST | ONLCR;
            set_modes = 1;
            continue;
        }
        for (f = 0; f < NFLAGS; f++) {
            if (strcmp(a + neg, flags[f].name) == 0) {
                if (neg) {
                    *field(&t, flags[f].which) &= ~flags[f].bit;
                } else {
                    *field(&t, flags[f].which) |= flags[f].bit;
                }
                set_modes = 1;
                break;
            }
        }
        if (f == NFLAGS) {
            eputs("stty: unknown setting ");
            eputs(a);
            eputs("\n");
            usage();
            return 1;
        }
    }

    if (set_size) {
        w.ws_xpixel = 0;
        w.ws_ypixel = 0;
        err = ioctl(0, TIOCSWINSZ, (u32)&w);
        if (err < 0) {
            eputs("stty: cannot set the size\n");
            return 1;
        }
    }
    if (set_modes && tcsetattr(0, TCSANOW, &t) < 0) {
        eputs("stty: cannot set the modes\n");
        return 1;
    }
    return 0;
}
