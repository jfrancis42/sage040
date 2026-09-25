/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * winsize - TIOCGWINSZ, TIOCSWINSZ and SIGWINCH, per terminal.
 *
 * The screen and the serial line are INDEPENDENT terminals now, each
 * with its own size. A program is told the size of the terminal its
 * descriptor is open on -- no minimum-of-two-outputs any more, because a
 * terminal has one output. Run on the serial line, this sees 24x80 (the
 * line's own default, settable); it opens /dev/tty1 to see the screen's
 * 80x30, which no setting on the line can change.
 *
 * A change to a terminal's size sends SIGWINCH to that terminal's
 * foreground group, and only to it.
 *
 * Results are collected and printed at the end.
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

static int is(const struct winsize *w, int rows, int cols)
{
    return w->ws_row == rows && w->ws_col == cols;
}

int main(void)
{
    struct sigaction sa;
    struct winsize w, screen;
    int before, pid, st, fd;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, 0);

    get(&w);
    note("TIOCGWINSZ on the serial line is 24x80, its own default",
         is(&w, 24, 80));
    note("  and the serial line reports no pixel size",
         w.ws_xpixel == 0 && w.ws_ypixel == 0);

    /* The screen is a different terminal: ask it through its own node.
     * No console to switch off, and no SIGWINCH here -- touching the
     * screen's size does not touch this terminal's. */
    fd = open("/dev/tty1", O_RDWR);
    if (fd >= 0) {
        memset(&screen, 0, sizeof(screen));
        ioctl(fd, TIOCGWINSZ, (u32)&screen);
        close(fd);
    } else {
        memset(&screen, 0, sizeof(screen));
    }
    note("the screen (/dev/tty1) is 80x30, measured by itself",
         is(&screen, 30, 80));
    note("  and reports its pixels, 640x480",
         screen.ws_xpixel == 640 && screen.ws_ypixel == 480);

    before = winches;
    note("TIOCSWINSZ succeeds", set_line(20, 70) == 0);
    get(&w);
    note("  and a smaller line is what programs are told", is(&w, 20, 70));
    note("  with one SIGWINCH to the foreground", winches - before == 1);

    before = winches;
    set_line(20, 70);
    note("setting the same size again sends none", winches == before);

    before = winches;
    set_line(40, 100);
    get(&w);
    note("a line bigger than the screen is NOT clamped -- separate terminals",
         is(&w, 40, 100));
    note("  which changed the size, so SIGWINCH", winches - before == 1);

    /* Setting the line does not disturb the screen. */
    fd = open("/dev/tty1", O_RDWR);
    if (fd >= 0) {
        memset(&screen, 0, sizeof(screen));
        ioctl(fd, TIOCGWINSZ, (u32)&screen);
        close(fd);
    }
    note("  and the screen is still 80x30, untouched", is(&screen, 30, 80));

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
    note("the line is back to 24x80", is(&w, 24, 80));

    for (st = 0; st < nr; st++) {
        puts(oks[st] ? "  ok   " : "  FAIL ");
        puts(names[st]);
        putch('\n');
    }
    puts("winsize: done\n");
    return 0;
}
