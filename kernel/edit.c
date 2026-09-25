/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * edit.c - the line editor.
 *
 * Emacs keys, because that is what a shell has: ctrl-A and ctrl-E for
 * the ends of the line, ctrl-B and ctrl-F to move, ctrl-P and ctrl-N for
 * history, ctrl-R and ctrl-S to search it, ctrl-U and ctrl-K and ctrl-W
 * to delete. Arrow keys too -- both the serial line and the keyboard
 * send VT100 escape sequences, so there is one parser for them and it
 * does not care which.
 *
 * REDRAWING IS THE WHOLE DESIGN PROBLEM, and it is a problem because of
 * what this terminal is. There are two sinks: a serial line, where
 * bytes are nearly free, and a framebuffer console, where every
 * character is a hundred and twenty-eight pixels drawn one at a time.
 * Redrawing the line after each keystroke -- what a simple editor does,
 * and what every line editor on a fast terminal gets away with -- would
 * be perfectly correct and visibly, unusably slow on the screen.
 *
 * So nothing redraws that does not have to. Typing at the end of the
 * line, which is almost everything anybody does, echoes exactly one
 * character. Moving left is backspaces; moving right re-echoes the
 * characters passed over. Only an edit in the middle of the line
 * rewrites the tail, and only as far as the tail goes.
 *
 * And nothing uses an escape sequence to do it. The framebuffer console
 * has no cursor addressing -- it understands carriage return, backspace,
 * tab and newline, and that is all -- so every movement here is built
 * out of those four. That constraint turns out to cost nothing: a line
 * editor that only ever moves within one line does not need more.
 */
#include "edit.h"
#include "syscall.h"
#include "errno.h"
#include "string.h"

#define HIST_MAX    16
#define HIST_LINE   128

#define CTRL(x)     ((x) & 0x1f)
#define DEL         0x7f
#define ESC         0x1b

/* ---------------------------------------------------------------- */
/* History                                                           */
/* ---------------------------------------------------------------- */

/*
 * A ring, oldest overwritten. Sixteen lines is enough to find the thing
 * you typed a minute ago and small enough that it costs two kilobytes,
 * which on a machine with no swap is a real consideration.
 */
static char hist[HIST_MAX][HIST_LINE];
static int hist_count;          /* how many are valid, up to HIST_MAX */
static int hist_next;           /* where the next one goes            */

static char *hist_slot(int index)
{
    int pos;

    if (index < 0 || index >= hist_count) {
        return 0;
    }
    /* Index 0 is the oldest kept, so a caller can walk forwards. */
    pos = (hist_next - hist_count + index) % HIST_MAX;
    if (pos < 0) {
        pos += HIST_MAX;
    }
    return hist[pos];
}

int edit_history_count(void)
{
    return hist_count;
}

const char *edit_history_nth(int index)
{
    return hist_slot(index);
}

void edit_history_add(const char *line)
{
    const char *last;

    if (!line || !line[0]) {
        return;
    }
    /* A repeat of the line before it is never what anybody wants to
     * scroll back through, and `ls` three times running would otherwise
     * fill a fifth of the history. */
    last = hist_slot(hist_count - 1);
    if (last && strcmp(last, line) == 0) {
        return;
    }
    strncpy(hist[hist_next], line, HIST_LINE - 1);
    hist[hist_next][HIST_LINE - 1] = '\0';
    hist_next = (hist_next + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) {
        hist_count++;
    }
}

/* ---------------------------------------------------------------- */
/* Output                                                            */
/* ---------------------------------------------------------------- */

/*
 * Everything the editor draws goes through one buffer and one write.
 *
 * Not for speed on the wire -- for the screen. A redraw of a forty
 * character line is a hundred small writes if each one is a system
 * call, and on the framebuffer console each of those turns double
 * buffering off and puts the cursor back. One write is one pass.
 */
#define OUT_MAX 512

static char outbuf[OUT_MAX];
static int  outlen;

static void o_flush(void)
{
    if (outlen > 0) {
        sys_write(STDOUT_FILENO, outbuf, (u32)outlen);
        outlen = 0;
    }
}

static void o_putc(char c)
{
    if (outlen == OUT_MAX) {
        o_flush();
    }
    outbuf[outlen++] = c;
}

static void o_puts(const char *s)
{
    while (*s) {
        o_putc(*s++);
    }
}

static void o_repeat(char c, int n)
{
    while (n-- > 0) {
        o_putc(c);
    }
}

/* n characters of buf, which may contain no control characters -- the
 * editor refuses them on the way in, so this never has to. */
static void o_write(const char *buf, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        o_putc(buf[i]);
    }
}

/* ---------------------------------------------------------------- */
/* The line being edited                                             */
/* ---------------------------------------------------------------- */

struct line {
    char *buf;
    int   max;                  /* including the terminator           */
    int   len;
    int   pos;                  /* where the cursor is, 0..len        */
};

/*
 * Move the cursor without redrawing anything.
 *
 * Left is backspaces, which every terminal understands. Right is the
 * characters being passed over, written again -- they are already on
 * the screen and identical, so writing them is invisible and is the
 * only way to move right without cursor addressing.
 */
static void move_to(struct line *l, int to)
{
    if (to < 0) {
        to = 0;
    }
    if (to > l->len) {
        to = l->len;
    }
    if (to < l->pos) {
        o_repeat('\b', l->pos - to);
    } else if (to > l->pos) {
        o_write(l->buf + l->pos, to - l->pos);
    }
    l->pos = to;
}

/*
 * Put the tail of the line back on the screen after the text changed.
 *
 * `erase` is how many characters the line lost, and therefore how many
 * stale ones are still sitting on the screen past the new end. Writing
 * spaces over exactly those and no more is what keeps this cheap: the
 * cost is the length of the tail, not the length of the line, so editing
 * near the end is nearly free and only editing at the very front costs
 * anything at all.
 */
static void redraw_tail(struct line *l, int erase)
{
    int tail = l->len - l->pos;

    o_write(l->buf + l->pos, tail);
    o_repeat(' ', erase);
    o_repeat('\b', tail + erase);
}

static void insert(struct line *l, char c)
{
    int i;

    if (l->len + 1 >= l->max) {
        return;                 /* full: refuse rather than overrun */
    }
    for (i = l->len; i > l->pos; i--) {
        l->buf[i] = l->buf[i - 1];
    }
    l->buf[l->pos] = c;
    l->len++;
    l->pos++;
    l->buf[l->len] = '\0';

    if (l->pos == l->len) {
        o_putc(c);              /* the common case: one character */
    } else {
        o_putc(c);
        redraw_tail(l, 0);
    }
}

/* Delete `n` characters ending at the cursor. */
static void delete_back(struct line *l, int n)
{
    int i;

    if (n > l->pos) {
        n = l->pos;
    }
    if (n <= 0) {
        return;
    }
    for (i = l->pos; i <= l->len; i++) {
        l->buf[i - n] = l->buf[i];
    }
    l->len -= n;
    l->pos -= n;

    o_repeat('\b', n);
    if (l->pos == l->len) {
        o_repeat(' ', n);       /* nothing after it: just rub it out */
        o_repeat('\b', n);
    } else {
        redraw_tail(l, n);
    }
}

/* Delete `n` characters starting at the cursor. */
static void delete_fwd(struct line *l, int n)
{
    int i;

    if (n > l->len - l->pos) {
        n = l->len - l->pos;
    }
    if (n <= 0) {
        return;
    }
    for (i = l->pos + n; i <= l->len; i++) {
        l->buf[i - n] = l->buf[i];
    }
    l->len -= n;
    redraw_tail(l, n);
}

/* Where the word before the cursor starts, the way ctrl-W counts one:
 * skip the spaces, then the run of non-spaces. */
static int word_start(struct line *l)
{
    int i = l->pos;

    while (i > 0 && l->buf[i - 1] == ' ') {
        i--;
    }
    while (i > 0 && l->buf[i - 1] != ' ') {
        i--;
    }
    return i;
}

/* Where the word after the cursor ends: skip the spaces, then the run of
 * non-spaces -- the forward mirror of word_start, used by Meta-f and the
 * forward word-delete of Meta-d. */
static int word_end(struct line *l)
{
    int i = l->pos;

    while (i < l->len && l->buf[i] == ' ') {
        i++;
    }
    while (i < l->len && l->buf[i] != ' ') {
        i++;
    }
    return i;
}

/* Replace the whole line, used by history and search. */
static void set_line(struct line *l, const char *s)
{
    int old = l->len;
    int n = 0;

    move_to(l, 0);
    while (s[n] && n < l->max - 1) {
        l->buf[n] = s[n];
        n++;
    }
    l->buf[n] = '\0';
    l->len = n;
    l->pos = n;

    o_write(l->buf, n);
    if (old > n) {
        o_repeat(' ', old - n);
        o_repeat('\b', old - n);
    }
}

/* ---------------------------------------------------------------- */
/* Input                                                             */
/* ---------------------------------------------------------------- */

/*
 * One character, or a negative errno.
 *
 * -EINTR comes back from the kernel's terminal when ctrl-C arrives, the
 * same as a real read interrupted by a signal, and it is how ctrl-C
 * reaches this loop at all -- the character itself never arrives,
 * because the terminal ate it and raised something instead.
 */
static int get_char(void)
{
    u8 c;
    s32 n = sys_read(STDIN_FILENO, &c, 1);

    if (n == 1) {
        return (int)c;
    }
    if (n < 0) {
        return (int)n;
    }
    return -EIO;                /* end of input */
}

/*
 * Keys that arrive as escape sequences, turned into one number each.
 *
 * Both the serial line and the keyboard send VT100 sequences, so this is
 * the only place either of them is decoded. An unrecognised sequence is
 * swallowed rather than typed into the line: a function key producing
 * three stray characters in the middle of a command is worse than one
 * that does nothing.
 */
#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_RIGHT   0x102
#define KEY_LEFT    0x103
#define KEY_HOME    0x104
#define KEY_END     0x105
#define KEY_DELETE  0x106
#define KEY_NONE    0x107
#define KEY_WFWD    0x108       /* Meta-f: forward one word   */
#define KEY_WBACK   0x109       /* Meta-b: back one word      */
#define KEY_WDEL    0x10a       /* Meta-d: delete a word ahead */

static int read_escape(void)
{
    int c = get_char();

    if (c < 0) {
        return c;
    }
    if (c != '[' && c != 'O') {
        /*
         * ESC then a letter is Meta-<letter> -- what Alt-f, Alt-b and
         * Alt-d send, on the serial line from the terminal and on the
         * screen from the keyboard driver, which prefixes ESC for Alt.
         * The word motions readline gives these keys; anything else is
         * swallowed rather than typed into the line.
         */
        switch (c) {
        case 'f': return KEY_WFWD;
        case 'b': return KEY_WBACK;
        case 'd': return KEY_WDEL;
        default:  return KEY_NONE;
        }
    }
    c = get_char();
    if (c < 0) {
        return c;
    }
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default:  break;
    }
    if (c >= '0' && c <= '9') {
        int n = c - '0';

        /* The numeric form, ESC [ n ~ -- delete is 3, home and end have
         * spellings here too on some terminals. */
        for (;;) {
            c = get_char();
            if (c < 0) {
                return c;
            }
            if (c == '~') {
                break;
            }
            if (c < '0' || c > '9') {
                return KEY_NONE;
            }
            n = n * 10 + (c - '0');
        }
        switch (n) {
        case 1:
        case 7:  return KEY_HOME;
        case 3:  return KEY_DELETE;
        case 4:
        case 8:  return KEY_END;
        default: return KEY_NONE;
        }
    }
    return KEY_NONE;
}

static int get_key(void)
{
    int c = get_char();

    if (c == ESC) {
        return read_escape();
    }
    return c;
}

/* ---------------------------------------------------------------- */
/* Searching the history                                             */
/* ---------------------------------------------------------------- */

static int contains(const char *hay, const char *needle)
{
    int i, j;

    if (!needle[0]) {
        return 1;
    }
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++) {
        }
        if (!needle[j]) {
            return 1;
        }
    }
    return 0;
}

/*
 * Incremental search, ctrl-R backwards and ctrl-S forwards.
 *
 * The prompt is replaced while it runs, which is what bash does and
 * which is also the only thing that can be done on a terminal with one
 * line of cursor control. Pressing ctrl-R again steps to the next older
 * match; anything that is not a search key accepts the match and is then
 * handled as an ordinary keystroke, so ctrl-A after a search moves to
 * the start of the line it found rather than being swallowed.
 *
 * Returns the key that ended the search, or a negative errno.
 */
static int search(struct line *l, const char *prompt, int backwards)
{
    char pattern[64];
    int plen = 0;
    int found = -1;
    int dir = backwards ? -1 : 1;
    int key;

    pattern[0] = '\0';

    for (;;) {
        const char *shown;

        /* Draw the search prompt and the current match over the line. */
        o_putc('\r');
        o_puts(backwards ? "(reverse-i-search)`" : "(i-search)`");
        o_puts(pattern);
        o_puts("': ");

        shown = (found >= 0) ? hist_slot(found) : l->buf;
        o_puts(shown ? shown : "");
        /*
         * Wipe whatever the previous, longer, state left behind. The
         * prompt grows and the match changes length, so the tail of the
         * old line is still sitting there otherwise.
         */
        o_repeat(' ', 8);
        o_flush();

        key = get_key();
        if (key < 0) {
            return key;
        }

        if (key == CTRL('R') || key == CTRL('S')) {
            int step = (key == CTRL('R')) ? -1 : 1;
            int i = (found >= 0) ? found + step
                                 : (step < 0 ? hist_count - 1 : 0);

            backwards = step < 0;
            dir = step;
            while (i >= 0 && i < hist_count) {
                if (contains(hist_slot(i), pattern)) {
                    found = i;
                    break;
                }
                i += step;
            }
            continue;
        }

        if (key == CTRL('G')) {
            /* Abandon the search and the match with it, back to the
             * line as it was. */
            found = -1;
            break;
        }

        if (key == '\b' || key == DEL) {
            if (plen > 0) {
                pattern[--plen] = '\0';
                found = -1;
            }
            continue;
        }

        if ((key >= 32 && key < 127) || (key >= 0x80 && key < 0x100)) {
            int i;

            if (plen < (int)sizeof(pattern) - 1) {
                pattern[plen++] = (char)key;
                pattern[plen] = '\0';
            }
            /* Extend the current match if it still fits, otherwise walk
             * on from where we are -- which is what makes it feel
             * incremental rather than like a fresh search each time. */
            i = (found >= 0) ? found : (dir < 0 ? hist_count - 1 : 0);
            found = -1;
            while (i >= 0 && i < hist_count) {
                if (contains(hist_slot(i), pattern)) {
                    found = i;
                    break;
                }
                i += dir;
            }
            continue;
        }

        break;                  /* anything else ends the search */
    }

    /*
     * Put the real prompt back, with whatever the search settled on.
     * The line is redrawn from scratch here, once, because the search
     * prompt was a different length and there is nothing to be clever
     * about.
     */
    o_putc('\r');
    o_puts(prompt);
    if (found >= 0) {
        const char *s = hist_slot(found);
        int n = 0;

        while (s[n] && n < l->max - 1) {
            l->buf[n] = s[n];
            n++;
        }
        l->buf[n] = '\0';
        l->len = n;
    }
    l->pos = l->len;
    o_write(l->buf, l->len);
    o_repeat(' ', 24);          /* over the rest of the search prompt */
    o_repeat('\b', 24);
    o_flush();

    return key;
}

/* ---------------------------------------------------------------- */
/* The editor                                                        */
/* ---------------------------------------------------------------- */

/*
 * Raw mode, exactly as readline does it: canonical mode and echo off,
 * SIGNALS LEFT ON. Turning ISIG off as well is the tempting mistake --
 * it looks like "give me every keystroke" -- and it would mean ctrl-C no
 * longer interrupted anything, which is the one thing the terminal must
 * never stop doing.
 */
static int raw_on(struct termios *saved)
{
    struct termios raw;

    if (sys_ioctl(STDIN_FILENO, TCGETS, (u32)saved) < 0) {
        return -1;
    }
    raw = *saved;
    raw.c_lflag &= ~(u32)(ICANON | ECHO);
    return sys_ioctl(STDIN_FILENO, TCSETS, (u32)&raw);
}

static void raw_off(const struct termios *saved)
{
    sys_ioctl(STDIN_FILENO, TCSETS, (u32)saved);
}

int edit_readline(const char *prompt, char *buf, int max)
{
    struct termios saved;
    struct line l;
    int hist_pos = hist_count;  /* one past the newest: the live line */
    char stash[HIST_LINE];      /* the line being typed, while browsing */
    int result;

    buf[0] = '\0';
    stash[0] = '\0';

    l.buf = buf;
    l.max = max;
    l.len = 0;
    l.pos = 0;

    o_puts(prompt);
    o_flush();

    /*
     * If the terminal will not go raw there is nothing to edit with, so
     * fall back to the kernel's own line assembly. That is what happens
     * when input is a file or a pipe rather than a terminal, which is
     * exactly how the test harnesses drive this machine -- and it is why
     * they kept working when the editor arrived.
     */
    if (raw_on(&saved) < 0) {
        s32 n = sys_read(STDIN_FILENO, buf, (u32)max - 1);

        if (n < 0) {
            return (int)n;
        }
        if (n == 0) {
            return -EIO;
        }
        if (buf[n - 1] == '\n') {
            n--;
        }
        buf[n] = '\0';
        return (int)n;
    }

    for (;;) {
        int key = get_key();

        if (key == -EINTR) {
            /* ctrl-C. Show it the way a shell does, throw the line
             * away, and let the caller print a fresh prompt. */
            o_puts("^C\n");
            result = -EINTR;
            goto done;
        }
        if (key < 0) {
            result = key;
            goto done;
        }

        switch (key) {
        case '\r':
        case '\n':
            move_to(&l, l.len);
            o_putc('\n');
            result = l.len;
            goto done;

        case CTRL('A'):
        case KEY_HOME:
            move_to(&l, 0);
            break;

        case CTRL('E'):
        case KEY_END:
            move_to(&l, l.len);
            break;

        case CTRL('B'):
        case KEY_LEFT:
            move_to(&l, l.pos - 1);
            break;

        case CTRL('F'):
        case KEY_RIGHT:
            move_to(&l, l.pos + 1);
            break;

        case CTRL('U'):
            /* Everything before the cursor, which is what bash's
             * unix-line-discard does -- not the whole line. */
            delete_back(&l, l.pos);
            break;

        case CTRL('K'):
            delete_fwd(&l, l.len - l.pos);
            break;

        case CTRL('W'):
            delete_back(&l, l.pos - word_start(&l));
            break;

        case KEY_WBACK:                 /* Meta-b: back a word */
            move_to(&l, word_start(&l));
            break;

        case KEY_WFWD:                  /* Meta-f: forward a word */
            move_to(&l, word_end(&l));
            break;

        case KEY_WDEL:                  /* Meta-d: delete the word ahead */
            delete_fwd(&l, word_end(&l) - l.pos);
            break;

        case CTRL('L'):
            /*
             * Clear the screen and put the line back, which is what
             * bash does -- the line being edited is not lost, it is
             * redrawn at the top. Anything else would make ctrl-L a
             * destructive key.
             */
            o_puts("\033[H\033[2J");
            o_puts(prompt);
            o_write(l.buf, l.len);
            {
                int back = l.len - l.pos;

                o_repeat('\b', back);
            }
            break;

        case '\b':
        case DEL:
            delete_back(&l, 1);
            break;

        case KEY_DELETE:
            delete_fwd(&l, 1);
            break;

        case CTRL('D'):
            if (l.len == 0) {
                /* End of input, on an empty line only -- mid-line it
                 * deletes, which is the other half of what ctrl-D has
                 * always meant. */
                result = -EIO;
                goto done;
            }
            delete_fwd(&l, 1);
            break;

        case CTRL('P'):
        case KEY_UP:
            if (hist_pos > 0) {
                if (hist_pos == hist_count) {
                    strncpy(stash, l.buf, HIST_LINE - 1);
                    stash[HIST_LINE - 1] = '\0';
                }
                set_line(&l, hist_slot(--hist_pos));
            }
            break;

        case CTRL('N'):
        case KEY_DOWN:
            if (hist_pos < hist_count) {
                hist_pos++;
                set_line(&l, hist_pos == hist_count
                             ? stash : hist_slot(hist_pos));
            }
            break;

        case CTRL('R'):
        case CTRL('S'): {
            int ended = search(&l, prompt, key == CTRL('R'));

            if (ended == -EINTR) {
                o_puts("^C\n");
                result = -EINTR;
                goto done;
            }
            if (ended < 0) {
                result = ended;
                goto done;
            }
            /* The key that ended the search is a real keystroke and is
             * handled as one -- except Enter, which submits. */
            if (ended == '\r' || ended == '\n') {
                o_putc('\n');
                result = l.len;
                goto done;
            }
            hist_pos = hist_count;
            break;
        }

        case KEY_NONE:
            break;              /* an escape sequence with no meaning */

        default:
            /* Bytes above 127 are kept: they are UTF-8, and a name
             * typed with an accent in it has to arrive with it. Each
             * byte counts as a column, which is right on the screen
             * (whose font is CP437, a glyph a byte) and over-counts on
             * a UTF-8 terminal -- editing in the middle of a line of
             * accented letters there can leave the display off by one
             * until the next redraw. */
            if ((key >= 32 && key < 127) || (key >= 0x80 && key < 0x100)) {
                insert(&l, (char)key);
            }
            /* Anything else is a control character with no binding.
             * Dropped rather than inserted: a stray 0x14 in the middle
             * of a command line is not what anybody meant. */
            break;
        }
        o_flush();
    }

done:
    o_flush();
    raw_off(&saved);
    return result;
}
