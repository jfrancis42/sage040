/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * winsize - TIOCGWINSZ, TIOCSWINSZ and SIGWINCH.
 *
 * The size a program is told is the smallest of the console's enabled
 * outputs: the screen's own, and the serial line's, which is 24x80
 * until TIOCSWINSZ says otherwise. A change to what a program would be
 * told -- by TIOCSWINSZ, or by an output being switched on or off --
 * sends SIGWINCH to the terminal's foreground group, and only to it.
 *
 * Results are collected and printed at the end, because part of the
 * test switches the serial line off.
 */
#include "ulib.h"

static volatile int winches;

static void on_winch(int sig)
{
    (void)sig;
    winches++;
}

#define MAXR 24
static const char *names[MAXR];
static int oks[MAXR];
static int nr;

static void note(const char *what, int ok)
{
    if (nr < MAXR) {
        names[nr] = what;
        oks[nr] = ok;
        nr++;
    }
}

static void get(struct winsize *w)
{
    memset(w, 0, sizeof(*w));
    ioctl(0, TIOCGWINSZ, (u32)w);
}

static int set_line(int rows, int cols)
{
    struct winsize w;

    memset(&w, 0, sizeof(w));
    w.ws_row = (u16)rows;
    w.ws_col = (u16)cols;
    return ioctl(0, TIOCSWINSZ, (u32)&w);
}

static int sink(const char *name, int on)
{
    struct console_set cs;
    u32 n = strlen(name);

    memset(&cs, 0, sizeof(cs));
    memcpy(cs.name, name, n + 1);
    cs.on = on;
    return ioctl(0, TIOCSCONS, (u32)&cs);
}

static int is(const struct winsize *w, int rows, int cols)
{
    return w->ws_row == rows && w->ws_col == cols;
}

int main(void)
{
    struct sigaction sa;
    struct winsize w, screen;
    int before, pid, st;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, 0);

    get(&w);
    note("TIOCGWINSZ at boot is 24x80: the serial line's default, "
         "smaller than the screen", is(&w, 24, 80));
    note("  with the pixel size of the smaller one left zero",
         w.ws_xpixel == 0 && w.ws_ypixel == 0);

    /* The screen alone, to learn its size. */
    before = winches;
    sink("ttyS0", 0);
    get(&screen);
    sink("ttyS0", 1);
    note("the screen alone is 30x80", is(&screen, 30, 80));
    note("  and says how many pixels that is",
         screen.ws_xpixel == 640 && screen.ws_ypixel == 480);
    note("switching the serial line off and on sent two SIGWINCHes",
         winches - before == 2);

    before = winches;
    note("TIOCSWINSZ succeeds", set_line(20, 70) == 0);
    get(&w);
    note("  and a smaller line is what programs are told",
         is(&w, 20, 70));
    note("  with one SIGWINCH to the foreground", winches - before == 1);

    before = winches;
    set_line(20, 70);
    note("setting the same size again sends none", winches == before);

    before = winches;
    set_line(40, 100);
    get(&w);
    note("a line bigger than the screen is limited by the screen",
         is(&w, 30, 80));
    note("  which changed the size, so SIGWINCH", winches - before == 1);

    before = winches;
    sink("fbcon", 0);
    get(&w);
    note("with the screen off, the line's own size", is(&w, 40, 100));
    sink("fbcon", 1);
    note("  and back, two more", winches - before == 2);

    before = winches;
    sink("fbcon", 0);
    set_line(30, 90);
    get(&w);
    note("TIOCSWINSZ with only the line on is exact", is(&w, 30, 90));
    sink("fbcon", 1);

    /* A background group is not told. */
    set_line(24, 80);
    pid = fork();
    if (pid == 0) {
        struct timespec ts;

        setpgid(0, 0);
        winches = 0;
        ts.tv_sec = 0;
        ts.tv_nsec = 600 * 1000 * 1000;
        nanosleep(&ts, 0);
        exit(winches);
    }
    {
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000 * 1000;
        nanosleep(&ts, 0);
    }
    before = winches;
    set_line(22, 80);
    waitpid(pid, &st, 0);
    note("a background group gets no SIGWINCH",
         WIFEXITED(st) && WEXITSTATUS(st) == 0);
    note("  while the foreground does", winches - before == 1);

    /* Its default action is to be ignored. */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGWINCH, &sa, 0);
    set_line(24, 80);
    note("SIGWINCH left at its default does not kill", 1);

    note("TIOCSWINSZ with a null pointer is EFAULT",
         ioctl(0, TIOCSWINSZ, 0) == -EFAULT);
    note("  and TIOCGWINSZ too", ioctl(0, TIOCGWINSZ, 0) == -EFAULT);
    get(&w);
    note("the size is back to 24x80", is(&w, 24, 80));

    for (st = 0; st < nr; st++) {
        puts(oks[st] ? "  ok   " : "  FAIL ");
        puts(names[st]);
        putch('\n');
    }
    puts("winsize: done\n");
    return 0;
}
