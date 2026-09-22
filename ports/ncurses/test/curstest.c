/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * curstest.c - terminfo and curses on SuckOS.
 *
 * WHAT THIS HAS TO PROVE, and the trap it is written around:
 *
 * ncurses is built here with four terminals COMPILED IN as fallbacks
 * (vt102, vt100, dumb, unknown), so that a program works on a disk with
 * no database at all. That means a test which only ever asks about
 * vt102 cannot tell a working database from a missing one -- it would
 * pass with /usr/share/terminfo deleted.
 *
 * So the checks come in pairs: something the fallback could answer, and
 * something ONLY THE DATABASE can (wyse50, xterm, vt220 -- none of them
 * compiled in). The second kind is the test of task 39.
 *
 * Then curses itself: a screen, cursor addressing, attributes, and the
 * bytes it actually sends, captured by writing to a pipe rather than to
 * a terminal, because what a curses program does IS the bytes it emits.
 */
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>
#include <unistd.h>

static int pass, fail;

static void
check(const char *what, int ok)
{
    if (ok) {
        pass++;
        printf("  [ OK ] %s\n", what);
    } else {
        fail++;
        printf("  [FAIL] %s\n", what);
    }
    fflush(stdout);
}

/* --- terminfo: the database, read at run time ------------------------ */

static void
test_terminfo(void)
{
    int err = 0;
    char *s;

    printf("=== terminfo ===\n");

    /* vt102 is a fallback AND in the database: it proves the calls
     * work, and nothing about where the answer came from. */
    check("setupterm(vt102)", setupterm("vt102", 1, &err) == OK);
    check("  80 columns, 24 lines", tigetnum("cols") == 80 &&
                                    tigetnum("lines") == 24);
    s = tigetstr("clear");
    check("  and knows how to clear the screen",
          s && s != (char *)-1 && strstr(s, "\033[") != 0);

    /*
     * wyse50 is NOT one of the compiled-in fallbacks, so an answer
     * about it can only have come from /usr/share/terminfo. This is
     * the check that the database is real; deleting the database makes
     * it, and only it, fail.
     */
    err = 0;
    check("setupterm(wyse50) -- not a fallback, so this is the database",
          setupterm("wyse50", 1, &err) == OK);
    check("  and its size is the Wyse's own (80x24)",
          tigetnum("cols") == 80 && tigetnum("lines") == 24);
    s = tigetstr("cup");
    check("  with cursor addressing that is not ANSI",
          s && s != (char *)-1 && strncmp(s, "\033[", 2) != 0);

    err = 0;
    check("setupterm(xterm) -- also only in the database",
          setupterm("xterm", 1, &err) == OK);
    check("  which says it has colours", tigetnum("colors") >= 8);

    /* A terminal that is in neither: the answer must be "no such
     * terminal", not a guess. ncurses sets errret to 0 for "no such
     * terminal" and -1 for "no database at all", and the difference
     * matters here -- with the database gone, every lookup above still
     * works from the fallbacks, and this is the one that changes. */
    {
        int r;
        char msg[96];

        err = 99;
        r = setupterm("no-such-terminal-here", 1, &err);
        snprintf(msg, sizeof(msg),
                 "a terminal that does not exist is refused (r=%d err=%d)",
                 r, err);
        check(msg, r == ERR);
        printf("  note: setupterm on a missing terminal gave errret %d\n", err);
    }

    /* termcap's names over the same database, which is what a program
     * written against <termcap.h> uses. */
    err = 0;
    setupterm("vt102", 1, &err);
    check("the termcap names work too (tgetnum, tgetstr)",
          tgetnum("co") == 80 && tgetstr("cl", 0) != 0);
}

/* --- curses: the bytes it sends -------------------------------------- */

/*
 * A curses program's output IS its behaviour, so this runs curses with
 * its output going to a FILE and then reads the file back. On a serial
 * console there is nothing else to look at -- and a check that only
 * asked curses what it believes the screen holds would pass just as
 * well with every escape sequence wrong.
 */
static void
test_curses(void)
{
    FILE *out;
    SCREEN *sc;
    char buf[4096];
    size_t n;
    int fd;

    printf("=== curses ===\n");

    out = fopen("/CURS.OUT", "w");
    if (!out) {
        check("a file for curses to write to", 0);
        return;
    }

    sc = newterm("vt102", out, stdin);
    check("newterm on a vt102", sc != 0);
    if (!sc) {
        fclose(out);
        return;
    }
    check("  the screen is the size terminfo says", COLS == 80 && LINES == 24);

    clear();
    mvaddstr(3, 5, "HELLO-CURSES");
    mvaddstr(10, 0, "second line");
    attron(A_REVERSE);
    mvaddstr(12, 0, "reversed");
    attroff(A_REVERSE);
    move(0, 0);
    refresh();

    endwin();
    delscreen(sc);
    fclose(out);

    fd = open("/CURS.OUT", O_RDONLY);
    n = fd >= 0 ? (size_t)read(fd, buf, sizeof(buf) - 1) : 0;
    if (fd >= 0) {
        close(fd);
    }
    buf[n] = '\0';

    check("it wrote something", n > 20);
    check("  the text is in it", strstr(buf, "HELLO-CURSES") != 0 &&
                                 strstr(buf, "second line") != 0);
    check("  addressed with escape sequences, not spaces",
          strstr(buf, "\033[") != 0);
    /*
     * Reverse video, as the terminal's own sgr string writes it: vt102
     * turns it on with ESC[0;7m rather than ESC[7m, because ncurses
     * uses the one capability that sets every attribute at once. The
     * claim is that the text sits between an on and an off, which is
     * what a program asking for A_REVERSE means -- not that a
     * particular byte sequence appears.
     */
    {
        const char *on = strstr(buf, "7m");
        const char *text = strstr(buf, "reversed");
        const char *off = text ? strstr(text, "\033[") : 0;

        check("  and reverse video was turned on before the text",
              on && text && on < text);
        check("  and turned off after it", off != 0);
    }

    /* The same program on a dumb terminal must NOT send escapes: that
     * is the whole point of asking the database rather than assuming. */
    out = fopen("/CURSD.OUT", "w");
    if (out) {
        sc = newterm("dumb", out, stdin);
        if (sc) {
            clear();
            mvaddstr(0, 0, "PLAIN-TEXT");
            refresh();
            endwin();
            delscreen(sc);
        }
        fclose(out);
        fd = open("/CURSD.OUT", O_RDONLY);
        n = fd >= 0 ? (size_t)read(fd, buf, sizeof(buf) - 1) : 0;
        if (fd >= 0) {
            close(fd);
        }
        buf[n] = '\0';
        check("on a dumb terminal the same program sends no escapes",
              strstr(buf, "PLAIN-TEXT") != 0 && strchr(buf, '\033') == 0);
    }
}

int
main(void)
{
    printf("curstest: terminfo and curses on SuckOS\n");
    test_terminfo();
    test_curses();
    printf("\n  passed: %d\n  failed: %d\n", pass, fail);
    printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
