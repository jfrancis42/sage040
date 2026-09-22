/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2026 Jeff Francis
 *
 * termcap.c - termcap, with one terminal compiled in.
 *
 * Sage040 has one console, and it is a VT102: the framebuffer console
 * emulates one, and anything on the serial line can be one. So there is
 * no database to read. tgetent() accepts the names that mean that
 * terminal and describes it; tgetnum("li") and ("co") ask the terminal
 * for its size, so a program that sizes itself from termcap gets the
 * real one.
 *
 * The cursor keys are described as the keyboard sends them, ESC [ A and
 * so on -- the VT102's normal mode -- and there is no "ks" to switch the
 * keypad into application mode, because the keyboard would not follow.
 */
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include "termcap.h"

char  PC;
char *UP;
char *BC;
short ospeed;

struct cap {
    const char *id;
    const char *val;
};

static const struct cap strings[] = {
    { "cl", "\033[H\033[J" },       /* clear the screen, home        */
    { "cm", "\033[%i%d;%dH" },      /* cursor to row, column         */
    { "ho", "\033[H" },
    { "ce", "\033[K" },             /* clear to end of line          */
    { "cd", "\033[J" },             /* clear to end of screen        */
    { "up", "\033[A" },
    { "do", "\033[B" },
    { "nd", "\033[C" },
    { "le", "\b" },
    { "bc", "\b" },
    { "cr", "\r" },
    { "nl", "\n" },
    { "sf", "\n" },                 /* scroll forward                */
    { "sr", "\033M" },              /* scroll reverse                */
    { "cs", "\033[%i%d;%dr" },      /* scrolling region              */
    { "al", "\033[L" },             /* insert a line                 */
    { "dl", "\033[M" },             /* delete a line                 */
    { "AL", "\033[%dL" },
    { "DL", "\033[%dM" },
    { "dc", "\033[P" },             /* delete a character            */
    { "DC", "\033[%dP" },
    { "IC", "\033[%d@" },
    { "im", "\033[4h" },            /* insert mode                   */
    { "ei", "\033[4l" },
    { "so", "\033[7m" },            /* standout: reverse             */
    { "se", "\033[m" },
    { "us", "\033[4m" },            /* underline                     */
    { "ue", "\033[m" },
    { "md", "\033[1m" },            /* bold                          */
    { "mr", "\033[7m" },
    { "me", "\033[m" },
    { "sc", "\0337" },              /* save and restore the cursor   */
    { "rc", "\0338" },
    { "ku", "\033[A" },
    { "kd", "\033[B" },
    { "kr", "\033[C" },
    { "kl", "\033[D" },
    { "kh", "\033[H" },
    { "@7", "\033[F" },             /* end                           */
    { "kD", "\033[3~" },            /* delete                        */
    { "kP", "\033[5~" },            /* page up                       */
    { "kN", "\033[6~" },            /* page down                     */
    { 0, 0 }
};

static const char *const flags[] = {
    "am",                           /* wraps at the right margin     */
    "xn",                           /* ...late: the VT100 glitch     */
    "bs",                           /* backspace moves left          */
    "mi",                           /* moves safely in insert mode   */
    "ms",                           /* moves safely in standout      */
    "km",
    0
};

static int terminal_known(const char *name)
{
    static const char *const names[] = {
        "vt102", "vt100", "vt220", "ansi", "xterm", "linux", "sage040", 0
    };
    int i;

    for (i = 0; names[i]; i++) {
        if (strcmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int tgetent(char *bp, const char *name)
{
    if (bp) {
        bp[0] = '\0';               /* nothing to copy: it is compiled in */
    }
    if (!name) {
        name = getenv("TERM");
    }
    return (name && terminal_known(name)) ? 1 : 0;
}

int tgetflag(const char *id)
{
    int i;

    for (i = 0; flags[i]; i++) {
        if (strcmp(id, flags[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int tgetnum(const char *id)
{
    struct winsize w;
    int have = ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row && w.ws_col;

    if (strcmp(id, "li") == 0) {
        return have ? w.ws_row : 24;
    }
    if (strcmp(id, "co") == 0) {
        return have ? w.ws_col : 80;
    }
    if (strcmp(id, "it") == 0) {
        return 8;                   /* tab stops every 8 */
    }
    return -1;
}

char *tgetstr(const char *id, char **area)
{
    int i;

    for (i = 0; strings[i].id; i++) {
        if (strcmp(id, strings[i].id) == 0) {
            size_t n = strlen(strings[i].val) + 1;
            char *out;

            if (!area || !*area) {
                return (char *)strings[i].val;
            }
            out = *area;
            memcpy(out, strings[i].val, n);
            *area += n;
            return out;
        }
    }
    return 0;
}

/*
 * The termcap % escapes a VT102's strings use, and the rest of the
 * common ones: %d %2 %3 decimal, %. a byte, %+x a byte plus x, %i one
 * added to both, %r the two swapped, %% a percent. The first parameter
 * is the row, as in every termcap entry, unless %r says otherwise.
 */
char *tgoto(const char *cap, int col, int row)
{
    static char out[64];
    int params[2];
    int p = 0, o = 0;

    if (!cap) {
        return "OOPS";              /* what historical tgoto returned */
    }
    params[0] = row;
    params[1] = col;
    while (*cap && o < (int)sizeof(out) - 8) {
        if (*cap != '%') {
            out[o++] = *cap++;
            continue;
        }
        cap++;
        switch (*cap++) {
        case 'd': case '2': case '3': {
            char digits[12];
            int n = 0, v = params[p < 2 ? p : 1], min;

            min = (cap[-1] == '2') ? 2 : (cap[-1] == '3') ? 3 : 1;
            p++;
            if (v < 0) {
                v = 0;
            }
            do {
                digits[n++] = (char)('0' + v % 10);
                v /= 10;
            } while (v || n < min);
            while (n) {
                out[o++] = digits[--n];
            }
            break;
        }
        case '.':
            out[o++] = (char)params[p < 2 ? p++ : 1];
            break;
        case '+':
            out[o++] = (char)(params[p < 2 ? p++ : 1] + *cap++);
            break;
        case 'i':
            params[0]++;
            params[1]++;
            break;
        case 'r': {
            int t = params[0];

            params[0] = params[1];
            params[1] = t;
            break;
        }
        case '%':
            out[o++] = '%';
            break;
        default:
            return "OOPS";
        }
    }
    out[o] = '\0';
    return out;
}

/* No padding: nothing here is slow enough to need it. Leading padding
 * digits, termcap's, are skipped. */
int tputs(const char *str, int affcnt, int (*putc)(int))
{
    (void)affcnt;
    if (!str) {
        return -1;
    }
    while ((*str >= '0' && *str <= '9') || *str == '.' || *str == '*') {
        str++;
    }
    while (*str) {
        putc((unsigned char)*str++);
    }
    return 0;
}
