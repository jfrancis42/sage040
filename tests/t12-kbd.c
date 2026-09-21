/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * t12-kbd.c - Intel 8042 keyboard controller.
 *
 * References: Intel 8042 datasheet; IBM PC/AT Technical Reference for
 * the scancode sets.
 *
 * The controller half of this is ordinary register poking. The half
 * worth testing is the one that is easy to get wrong and impossible to
 * notice: WHICH SCANCODE SET arrives.
 *
 * A PS/2 keyboard powers up in set 2, where a key release is the prefix
 * 0xF0 followed by the make code. The 8042 can translate set 2 to set 1
 * on the way past -- bit 6 of the command byte -- and set 1 is what the
 * PC's own BIOS always saw: a release is the make code with bit 7 set.
 *
 * QEMU resets the controller with translation OFF, where a real PC
 * arrives with the BIOS having already turned it on. So a driver written
 * from a PC reference, tried here, decodes nonsense -- and nonsense that
 * looks like a broken keymap rather than like the wrong scancode set.
 * This test pins the distinction down: 'a' pressed and released must
 * arrive as 0x1E then 0x9E, which is set 1. In set 2 it would be 0x1C
 * then 0xF0 0x1C, and the check would fail loudly rather than the
 * machine merely typing the wrong letters.
 *
 * Keystrokes are injected by the harness through QEMU's monitor once
 * this prints KEYS-PLEASE. Nothing here can type on its own.
 */
#include "sage040.h"

#define WAIT_SPIN   200000
#define MAX_CODES   32

static u8 codes[MAX_CODES];
static int ncodes;

static int wait_input_clear(void)
{
    long spin;

    for (spin = 0; spin < WAIT_SPIN; spin++) {
        if (!(MMIO8(KBD_STATUS) & KBD_STAT_IBF)) {
            return 0;
        }
    }
    return -1;
}

static int wait_output_full(void)
{
    long spin;

    for (spin = 0; spin < WAIT_SPIN; spin++) {
        if (MMIO8(KBD_STATUS) & KBD_STAT_OBF) {
            return 0;
        }
    }
    return -1;
}

static int command(u8 c)
{
    if (wait_input_clear() != 0) {
        return -1;
    }
    MMIO8(KBD_COMMAND) = c;
    return 0;
}

static int write_data(u8 v)
{
    if (wait_input_clear() != 0) {
        return -1;
    }
    MMIO8(KBD_DATA) = v;
    return 0;
}

static int read_data(u8 *out)
{
    if (wait_output_full() != 0) {
        return -1;
    }
    *out = MMIO8(KBD_DATA);
    return 0;
}

static int saw(u8 code)
{
    int i;

    for (i = 0; i < ncodes; i++) {
        if (codes[i] == code) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    u8 v = 0, mode = 0;
    long spin;

    test_begin("t12 Intel 8042 keyboard controller");

    /* --- the controller answers its own self test ------------------ */
    if (command(KBD_CCMD_SELF_TEST) == 0 && read_data(&v) == 0) {
        uart_puts("  self test   = 0x"); uart_puthex8(v); uart_putc('\n');
        if (v == KBD_SELF_TEST_OK) {
            test_ok("self test returns 0x55");
        } else {
            test_fail("self test did not return 0x55");
        }
    } else {
        test_fail("the controller never answered - is it there?");
        test_end();
        return 0;
    }

    /* --- the command byte is readable and writable ----------------- */
    if (command(KBD_CCMD_READ_MODE) != 0 || read_data(&mode) != 0) {
        test_fail("could not read the command byte");
        test_end();
        return 0;
    }
    uart_puts("  command byte= 0x"); uart_puthex8(mode); uart_putc('\n');

    /*
     * Translation is off out of reset. Saying so is the point of this
     * check: a driver that assumes a PC's state gets set 2 here.
     */
    if (!(mode & KBD_MODE_TRANSLATE)) {
        test_ok("translation starts OFF, unlike a BIOS-initialised PC");
    } else {
        test_fail("translation was already on - the premise has changed");
    }

    mode |= KBD_MODE_TRANSLATE;
    mode &= (u8)~(KBD_MODE_DISABLE_KBD | KBD_MODE_KBD_INT);

    if (command(KBD_CCMD_WRITE_MODE) != 0 || write_data(mode) != 0) {
        test_fail("could not write the command byte");
        test_end();
        return 0;
    }

    v = 0;
    if (command(KBD_CCMD_READ_MODE) == 0 && read_data(&v) == 0 &&
        (v & KBD_MODE_TRANSLATE)) {
        test_ok("translation bit was written and read back");
    } else {
        test_fail("the translation bit did not stick");
    }

    if (command(KBD_CCMD_KBD_ENABLE) == 0) {
        test_ok("keyboard interface enabled");
    } else {
        test_fail("could not enable the keyboard interface");
    }

    /* --- collect what the harness types ---------------------------- */
    uart_puts("KEYS-PLEASE\n");

    for (spin = 0; spin < 40000000L && ncodes < MAX_CODES; spin++) {
        if (MMIO8(KBD_STATUS) & KBD_STAT_OBF) {
            u8 c = MMIO8(KBD_DATA);

            if (!(MMIO8(KBD_STATUS) & KBD_STAT_AUX)) {
                codes[ncodes++] = c;
            }
            /* Four codes is 'a' down and up plus shift down and up,
             * which is everything this needs. */
            if (ncodes >= 6) {
                break;
            }
        }
    }

    uart_puts("  scancodes   =");
    {
        int i;

        for (i = 0; i < ncodes; i++) {
            uart_puts(" 0x");
            uart_puthex8(codes[i]);
        }
        uart_putc('\n');
    }

    if (ncodes > 0) {
        test_ok("scancodes arrived from the keyboard");
    } else {
        test_fail("no scancodes arrived - was a key sent?");
        test_end();
        return 0;
    }

    /*
     * 'a' is 0x1E in set 1 and 0x1C in set 2, so this distinguishes
     * them outright rather than by inference.
     */
    if (saw(0x1e)) {
        test_ok("'a' arrived as 0x1E, which is set 1");
    } else if (saw(0x1c)) {
        test_fail("'a' arrived as 0x1C - that is set 2, not set 1");
    } else {
        test_fail("'a' did not arrive at all");
    }

    /* A release is the make code with bit 7 set, and there is no 0xF0
     * prefix anywhere. Both are set 1 and neither is set 2. */
    if (saw(0x9e)) {
        test_ok("release is 0x9E, the make code with bit 7 set");
    } else {
        test_fail("no release code for 'a'");
    }

    if (!saw(0xf0)) {
        test_ok("no 0xF0 release prefix, so translation really is on");
    } else {
        test_fail("saw a 0xF0 prefix - the codes are still set 2");
    }

    if (saw(0x2a)) {
        test_ok("left shift arrived as 0x2A");
    } else {
        test_fail("left shift did not arrive");
    }

    test_end();
    return 0;
}
