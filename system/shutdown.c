/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * shutdown - stop the machine.
 *
 * This is the one program that has to reach all the way down to the
 * board, and it is worth following what happens, because the mechanism
 * is a genuine piece of the hardware rather than something the emulator
 * invented for convenience.
 *
 * reboot() traps into the kernel, which flushes the filesystem and then
 * asks the device model whether anything can cut the power. On this
 * machine something can: the Intel 8042 keyboard controller has a spare
 * output line, and on the IBM PC it was wired to the processor's RESET
 * pin because there was nowhere else to put it. Every PC since has
 * rebooted itself by telling its keyboard controller to do it, and this
 * board inherited the part and the trick with it.
 *
 * Under the emulator, started with -no-reboot, a guest-requested reset
 * ends the process -- so the machine stopping and the emulator exiting
 * are the same event, which is exactly what is wanted from a terminal
 * you cannot otherwise get out of.
 *
 *   shutdown         stop the machine
 *   shutdown -h      halt the processor and leave it sitting there
 *
 * The second is what `halt` has always done here, and is the useful one
 * when something is being watched from outside.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int halt_only = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'h' && argv[i][2] == '\0') {
            halt_only = 1;
        } else {
            eputs("usage: shutdown [-h]\n");
            eputs("  -h  halt the processor instead of stopping the machine\n");
            return 1;
        }
    }

    puts(halt_only ? "halting.\n" : "shutting down.\n");

    /*
     * Nothing after this is expected to run. It is here because "does
     * not return" is a claim about the hardware, and a program that
     * quietly fell through would leave the person looking at a prompt
     * wondering whether anything had happened at all.
     */
    reboot(halt_only ? RB_HALT_SYSTEM : RB_POWER_OFF);

    eputs("shutdown: the machine refused to stop\n");
    return 1;
}
