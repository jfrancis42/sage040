/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * reboot - restart the machine.
 *
 * The companion to `shutdown`, and the honest use of the only mechanism
 * this board has for stopping itself: the Intel 8042 keyboard
 * controller's spare output line, wired to the processor's RESET pin
 * because on the IBM PC there was nowhere else to put it.
 *
 * That line resets; it does not cut power. `shutdown` asks to go away
 * and this board has to reset to do it, and says so. `reboot` asks for
 * exactly what the hardware does, so it says nothing.
 *
 * Under the emulator, started with -no-reboot, a guest-requested reset
 * ends the process instead of restarting it -- so under QEMU `reboot`
 * and `shutdown` look identical, and on real hardware they are not.
 *
 *   reboot      restart the machine
 *
 * The filesystem is flushed and unmounted on the way, by the kernel,
 * which is what marks the volume clean: a machine stopped any other way
 * is checked at the next boot.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc > 1) {
        eputs("usage: reboot\n");
        return 1;
    }
    (void)argv;

    puts("restarting.\n");

    /*
     * Nothing after this is expected to run. It is here because "does
     * not return" is a claim about the hardware, and a program that
     * quietly fell through would leave the person looking at a prompt
     * wondering whether anything had happened at all.
     */
    reboot(RB_AUTOBOOT);

    eputs("reboot: the machine refused to restart\n");
    return 1;
}
