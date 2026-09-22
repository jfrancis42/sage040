/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * dmesg - what the kernel has said since the last time anybody looked.
 *
 * It reads /dev/klog, which DRAINS: what this takes, klogd will not
 * see. That is deliberate and it is why, on a machine running klogd,
 * this prints nothing and /var/log/syslog has everything -- which is
 * the right answer, because a log that two readers each got half of
 * would be a log of neither.
 *
 * With no klogd running, this is how to see the boot messages after
 * the screen has scrolled.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[512];
    int  fd, follow = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            follow = 1;         /* keep reading, as `tail -f` does */
        } else {
            eputs("usage: dmesg [-f]\n");
            return 2;
        }
    }

    fd = open("/dev/klog", O_RDONLY);
    if (fd < 0) {
        eputs("dmesg: no /dev/klog\n");
        return 1;
    }

    /*
     * Without -f, stop when the ring is empty rather than waiting for
     * the kernel to say something else: O_NONBLOCK is what makes a read
     * with nothing to read return EAGAIN instead of sleeping.
     */
    if (!follow) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    for (;;) {
        s32 n = read(fd, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }
        write(1, buf, (u32)n);
    }
    close(fd);
    return 0;
}
