/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * devperm - what a device's mode lets THIS process open, one line per
 * device: "devperm PATH r|w|rw OK" or "... EACCES" (or another errno).
 * Run by kernel/devmodetest.sh as different people, which compares the
 * answers with what each device's mode, owner and group say they
 * should be.
 *
 *   devperm PATH MODE...
 *
 * MODE is r, w or rw, each tried with its own open.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: devperm PATH r|w|rw...\n");
        return 2;
    }
    for (i = 2; i < argc; i++) {
        int fl = strcmp(argv[i], "r") == 0 ? O_RDONLY :
                 strcmp(argv[i], "w") == 0 ? O_WRONLY : O_RDWR;
        int fd = open(argv[1], fl | O_NOCTTY | O_NONBLOCK);

        printf("devperm %s %s %s\n", argv[1], argv[i],
               fd >= 0 ? "OK" : errno == EACCES ? "EACCES" : strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
    }
    return 0;
}
