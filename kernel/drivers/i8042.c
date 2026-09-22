/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * i8042.c - the keyboard, an Intel 8042 at 0xff700000.
 *
 * References: Intel 8042 datasheet; IBM PC/AT Technical Reference for
 * the scancode tables.
 *
 * Registers as /dev/kbd0 and hands itself to the terminal layer as an
 * input source. Nothing above it knows there is a keyboard rather than a
 * serial line: both are places characters come from, and tty.c takes
 * them from whichever has one.
 *
 * SCANCODE SET 1, BY CHOICE. A PS/2 keyboard powers up in set 2, where a
 * key release is the release prefix 0xF0 followed by the make code. The
 * 8042 can translate set 2 to set 1 on the way past -- that is what bit
 * 6 of the command byte does -- and set 1 is what the PC's own BIOS
 * always saw: a release is simply the make code with bit 7 set, and
 * there is no prefix to track. Translation is switched on in kbd_init()
 * for that reason, and nothing here would work if it were not.
 *
 * Worth knowing when reading this against a real PC: QEMU resets the
 * controller with translation OFF and the keyboard in set 2, where a PC
 * arrives with the BIOS having already turned it on. Anything that does
 * not set it up gets set 2 codes and decodes nonsense.
 *
 * Polled, like the serial port, and for the same reason: it works before
 * interrupts are running and cannot deadlock against the code reporting
 * a fault. The controller's interrupt is wired to MFP GPIP1 and is the
 * obvious next step -- it would fill the same ring this already drains
 * into, so nothing above this file changes.
 */
#include "dev.h"
#include "tty.h"
#include "errno.h"
#include "vfs.h"
#include "drivers.h"

#define WAIT_SPIN      100000

#define RING_SIZE      32       /* power of two */
#define RING_MASK      (RING_SIZE - 1)

/* Scancodes this driver cares about by name rather than by character. */
#define SC_LSHIFT      0x2a
#define SC_RSHIFT      0x36
#define SC_LCTRL       0x1d
#define SC_CAPS        0x3a
#define SC_EXTENDED    0xe0
#define SC_BREAK       0x80

static u8 ring[RING_SIZE];
static u32 ring_head, ring_tail;

static int shift, ctrl, caps, extended;

/*
 * Scancode set 1 to ASCII, US layout.
 *
 * Index is the make code, 0x00 to 0x7f. Zero means the key produces no
 * character -- a function key, a modifier, something on the numeric pad
 * that needs num lock to mean anything. Those are dropped rather than
 * guessed at.
 */
static const char map_plain[128] = {
    0,    27,  '1',  '2',  '3',  '4',  '5',  '6',    /* 00-07 */
  '7',   '8',  '9',  '0',  '-',  '=', '\b', '\t',    /* 08-0f */
  'q',   'w',  'e',  'r',  't',  'y',  'u',  'i',    /* 10-17 */
  'o',   'p',  '[',  ']', '\n',    0,  'a',  's',    /* 18-1f */
  'd',   'f',  'g',  'h',  'j',  'k',  'l',  ';',    /* 20-27 */
 '\'',   '`',    0, '\\',  'z',  'x',  'c',  'v',    /* 28-2f */
  'b',   'n',  'm',  ',',  '.',  '/',    0,  '*',    /* 30-37 */
    0,   ' ',    0,    0,    0,    0,    0,    0,    /* 38-3f */
    0,     0,    0,    0,    0,    0,    0,  '7',    /* 40-47 */
  '8',   '9',  '-',  '4',  '5',  '6',  '+',  '1',    /* 48-4f */
  '2',   '3',  '0',  '.',    0,    0,    0,    0,    /* 50-57 */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 58-5f */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 60-67 */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 68-6f */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 70-77 */
    0,     0,    0,    0,    0,    0,    0,    0     /* 78-7f */
};

static const char map_shift[128] = {
    0,    27,  '!',  '@',  '#',  '$',  '%',  '^',    /* 00-07 */
  '&',   '*',  '(',  ')',  '_',  '+', '\b', '\t',    /* 08-0f */
  'Q',   'W',  'E',  'R',  'T',  'Y',  'U',  'I',    /* 10-17 */
  'O',   'P',  '{',  '}', '\n',    0,  'A',  'S',    /* 18-1f */
  'D',   'F',  'G',  'H',  'J',  'K',  'L',  ':',    /* 20-27 */
  '"',   '~',    0,  '|',  'Z',  'X',  'C',  'V',    /* 28-2f */
  'B',   'N',  'M',  '<',  '>',  '?',    0,  '*',    /* 30-37 */
    0,   ' ',    0,    0,    0,    0,    0,    0,    /* 38-3f */
    0,     0,    0,    0,    0,    0,    0,  '7',    /* 40-47 */
  '8',   '9',  '-',  '4',  '5',  '6',  '+',  '1',    /* 48-4f */
  '2',   '3',  '0',  '.',    0,    0,    0,    0,    /* 50-57 */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 58-5f */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 60-67 */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 68-6f */
    0,     0,    0,    0,    0,    0,    0,    0,    /* 70-77 */
    0,     0,    0,    0,    0,    0,    0,    0     /* 78-7f */
};

/* ---------------------------------------------------------------- */
/* Talking to the controller                                         */
/* ---------------------------------------------------------------- */

/* Wait for the controller to take what was last written to it. */
static int wait_input_clear(void)
{
    u32 spin;

    for (spin = 0; spin < WAIT_SPIN; spin++) {
        if (!(MMIO8(KBD_STATUS) & KBD_STAT_IBF)) {
            return 0;
        }
    }
    return -EIO;
}

static int wait_output_full(void)
{
    u32 spin;

    for (spin = 0; spin < WAIT_SPIN; spin++) {
        if (MMIO8(KBD_STATUS) & KBD_STAT_OBF) {
            return 0;
        }
    }
    return -EIO;
}

static int kbd_command(u8 cmd)
{
    if (wait_input_clear() < 0) {
        return -EIO;
    }
    MMIO8(KBD_COMMAND) = cmd;
    return 0;
}

static int kbd_write_data(u8 v)
{
    if (wait_input_clear() < 0) {
        return -EIO;
    }
    MMIO8(KBD_DATA) = v;
    return 0;
}

static int kbd_read_data(u8 *out)
{
    if (wait_output_full() < 0) {
        return -EIO;
    }
    *out = MMIO8(KBD_DATA);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Decoding                                                          */
/* ---------------------------------------------------------------- */

static void ring_put(u8 c)
{
    u32 next = (ring_head + 1) & RING_MASK;

    if (next == ring_tail) {
        /* Full. Drop the newest rather than the oldest: what someone
         * typed first is what they meant first. */
        return;
    }
    ring[ring_head] = c;
    ring_head = next;
}

/*
 * What an extended key sends.
 *
 * The same escape sequences a VT100 sends down a serial line, so that
 * everything above this driver sees one kind of terminal. The line
 * editor parses these once and works identically whether the person is
 * typing on the keyboard or on the other end of the wire -- which is
 * the whole reason to bother: writing a second, scancode-shaped path
 * for the same four arrow keys would be two things to keep in step.
 */
static const char *extended_seq(u8 code)
{
    switch (code) {
    case 0x48: return "\033[A";     /* up        */
    case 0x50: return "\033[B";     /* down      */
    case 0x4d: return "\033[C";     /* right     */
    case 0x4b: return "\033[D";     /* left      */
    case 0x47: return "\033[H";     /* home      */
    case 0x4f: return "\033[F";     /* end       */
    case 0x53: return "\033[3~";    /* delete    */
    case 0x49: return "\033[5~";    /* page up   */
    case 0x51: return "\033[6~";    /* page down */
    default:   return 0;
    }
}

/*
 * One scancode in, at most one character out.
 *
 * A key release, a modifier and the second half of an extended sequence
 * all produce nothing, which is why this cannot be a simple table lookup
 * at the point of reading -- there has to be somewhere to put the
 * characters that a given read does produce, and somewhere for a read
 * that produces none to leave things.
 */
static void decode(u8 code)
{
    int release;
    char c;

    if (code == SC_EXTENDED) {
        extended = 1;
        return;
    }

    release = (code & SC_BREAK) != 0;
    code &= (u8)~SC_BREAK;

    if (extended) {
        /*
         * Arrows, Home, the right-hand control and alt, and the keypad
         * divide, all arrive with the 0xE0 prefix.
         */
        const char *seq;

        extended = 0;
        if (code == SC_LCTRL) {
            ctrl = !release;        /* right control is E0 1D */
            return;
        }
        if (release) {
            return;
        }
        seq = extended_seq(code);
        if (seq) {
            while (*seq) {
                ring_put((u8)*seq++);
            }
        }
        return;
    }

    switch (code) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        shift = !release;
        return;
    case SC_LCTRL:
        ctrl = !release;
        return;
    case SC_CAPS:
        if (!release) {
            caps = !caps;           /* on the press, not the release */
        }
        return;
    default:
        break;
    }

    if (release) {
        return;                     /* nothing else cares about releases */
    }

    c = shift ? map_shift[code] : map_plain[code];
    if (c == 0) {
        return;
    }

    /* Caps lock is not shift: it applies to letters and nothing else. */
    if (caps) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        } else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
    }

    /*
     * Control characters. The terminal's line discipline is looking for
     * ctrl-U and ctrl-D, so this is not decoration -- without it there
     * is no way to kill a line or end input from the keyboard.
     */
    if (ctrl) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 1);
        } else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 1);
        } else {
            return;
        }
    }

    ring_put((u8)c);
}

/* Take everything the controller is holding and decode it. */
static void drain(void)
{
    u32 guard;

    for (guard = 0; guard < 64; guard++) {
        u8 status = MMIO8(KBD_STATUS);

        if (!(status & KBD_STAT_OBF)) {
            return;
        }
        if (status & KBD_STAT_AUX) {
            (void)MMIO8(KBD_DATA);  /* the mouse; nothing reads one */
            continue;
        }
        decode(MMIO8(KBD_DATA));
    }
}

/* ---------------------------------------------------------------- */
/* file_ops                                                          */
/* ---------------------------------------------------------------- */

static s32 kbd_read(struct file *f, void *buf, u32 len)
{
    u8 *out = buf;
    u32 n = 0;

    (void)f;
    if (len == 0) {
        return 0;
    }
    while (ring_head == ring_tail) {
        drain();                    /* blocks until a key produces one */
    }
    while (n < len && ring_head != ring_tail) {
        out[n++] = ring[ring_tail];
        ring_tail = (ring_tail + 1) & RING_MASK;
    }
    return (s32)n;
}

static int kbd_ioctl(struct file *f, u32 request, u32 arg)
{
    (void)f;

    switch (request) {
    case FIONREAD:
        /*
         * Decode whatever is waiting before answering, because a
         * scancode is not a character: a key release or a shift press
         * would otherwise have this say yes to a read that then blocks.
         */
        drain();
        if (arg) {
            *(u32 *)arg = (ring_head - ring_tail) & RING_MASK;
        }
        return 0;

    default:
        return -ENOTTY;
    }
}

static int kbd_close(struct file *f)
{
    (void)f;
    return 0;
}


/*
 * A character device has no size and no meaningful time; what a caller
 * actually wants from this is S_ISCHR, which is how isatty() is built.
 */
static int kbd_fstat(struct file *f, struct stat *st)
{
    (void)f;
    st->st_mode = S_IFCHR;
    st->st_size = 0;
    st->st_mtime = 0;
    st->st_blocks = 0;
    return 0;
}

static const struct file_ops kbd_ops = {
    kbd_read,
    0,                          /* a keyboard does not write */
    0,                          /* nor seek */
    kbd_ioctl,
    kbd_close,
    kbd_fstat,
    0,                          /* poll: the default; see dev.h */
    0,                          /* truncate: nothing to truncate */
};

static struct chardev kbd_dev = {
    "kbd0",
    &kbd_ops,
    0,
    0
};

/* ---------------------------------------------------------------- */

/*
 * Is the controller there?
 *
 * Its self test answers 0x55 and nothing else does. Reading the status
 * register would not do: an absent device reads as zeroes or ones, and
 * both are a plausible status byte.
 */
int i8042_present(void)
{
    u8 reply = 0;

    /*
     * Before anything else: is the controller there at all? An emulator
     * built without it faults on this address rather than reading back
     * zeroes, so asking the question by reading the status register --
     * which is what the rest of this function does -- is asking it too
     * late.
     */
    if (!io_probe8((volatile void *)KBD_STATUS)) {
        return 0;
    }

    if (kbd_command(KBD_CCMD_SELF_TEST) < 0) {
        return 0;
    }
    if (kbd_read_data(&reply) < 0) {
        return 0;
    }
    return reply == KBD_SELF_TEST_OK;
}

/*
 * Reset the machine through the keyboard controller.
 *
 * The 8042 has a spare output line, and on the IBM PC it was wired to
 * the processor's RESET pin because there was nowhere else to put it.
 * Every PC has rebooted this way since, and QEMU models it: with
 * -no-reboot a guest-requested reset ends the emulator, which is what
 * makes `shutdown` able to actually stop this machine.
 *
 * Registered with the device model rather than called by name, so that
 * reboot() does not have to know a keyboard is involved. On a board with
 * a real power controller, that driver registers instead and nothing
 * above here changes.
 *
 * Returns only if the pulse did not take.
 */
static int kbd_poweroff(void)
{
    if (kbd_command(KBD_CCMD_RESET) < 0) {
        return -EIO;
    }
    /*
     * Give it a moment. The reset is asynchronous -- the controller
     * pulls the line and the machine goes away underneath us -- and
     * returning "it worked" before it has is a race with nothing to win.
     */
    {
        volatile u32 spin;

        for (spin = 0; spin < 1000000; spin++) {
        }
    }
    return -EIO;                /* still here, so it did not */
}

static void kbd_isr(void *arg)
{
    (void)arg;
    tty_input_irq();
}

int i8042_init(void)
{
    u8 mode = 0;
    int err;

    if (!i8042_present()) {
        return -ENODEV;
    }

    /*
     * Read the command byte, turn on translation, leave the keyboard
     * enabled, and put it back. Translation is the important one: see
     * the note at the top about set 1 against set 2.
     *
     * The interrupt bit is left off HERE, and turned on at the end,
     * once there is a handler for MFP channel 1: an enabled source with
     * no handler is how you get an interrupt storm on the first key.
     */
    if (kbd_command(KBD_CCMD_READ_MODE) < 0 ||
        kbd_read_data(&mode) < 0) {
        return -EIO;
    }

    mode |= KBD_MODE_TRANSLATE;
    mode &= (u8)~(KBD_MODE_DISABLE_KBD | KBD_MODE_KBD_INT);

    if (kbd_command(KBD_CCMD_WRITE_MODE) < 0 ||
        kbd_write_data(mode) < 0) {
        return -EIO;
    }
    if (kbd_command(KBD_CCMD_KBD_ENABLE) < 0) {
        return -EIO;
    }

    ring_head = ring_tail = 0;
    shift = ctrl = caps = extended = 0;

    err = dev_register_char(&kbd_dev);
    if (err < 0) {
        return err;
    }

    /* The one thing on this board that can stop the machine. */
    dev_register_poweroff(kbd_poweroff);

    /* A source only. A keyboard is not somewhere output can go. */
    err = tty_add_source(&kbd_dev);
    if (err < 0) {
        return err;
    }

    /*
     * The interrupt (task 22): output-buffer-full raises IRQ1, wired to
     * MFP GPIP1. The handler drains the controller through the terminal
     * -- FIONREAD here decodes whatever is waiting -- which is also what
     * lowers the line again.
     */
    if (mfp_request_gpip(MFP_PIN_KBD, kbd_isr, 0) == 0 &&
        kbd_command(KBD_CCMD_WRITE_MODE) == 0 &&
        kbd_write_data((u8)(mode | KBD_MODE_KBD_INT)) == 0) {
        tty_source_irq(&kbd_dev);
    }
    return 0;
}

struct chardev *i8042_device(void)
{
    return &kbd_dev;
}
