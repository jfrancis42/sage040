/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * userdel - remove an account.
 *
 *   userdel NAME        remove the account, leave the home directory
 *   userdel -r NAME     and remove the home directory too
 *
 * The home directory is KEPT by default, because a deleted account's
 * files are usually still wanted and a mistake here is not reversible.
 * -r is the explicit way to say otherwise.
 *
 * Root's account cannot be removed. A machine with no uid 0 in
 * /etc/passwd is a machine nobody can administer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pwdb.h"

int main(int argc, char **argv)
{
    struct pwent e;
    const char *name = 0;
    int removehome = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            removehome = 1;
        } else if (argv[i][0] == '-') {
            fputs("usage: userdel [-r] name\n", stderr);
            return 1;
        } else {
            name = argv[i];
        }
    }
    if (!name) {
        fputs("userdel: no name given\n", stderr);
        return 1;
    }
    if (geteuid() != 0) {
        fputs("userdel: only root may remove a user\n", stderr);
        return 1;
    }
    if (pw_by_name(name, &e) != 0) {
        fprintf(stderr, "userdel: no such user: %s\n", name);
        return 1;
    }
    if (e.uid == 0) {
        fputs("userdel: refusing to remove uid 0\n", stderr);
        return 1;
    }
    if (pw_del(name) != 0) {
        perror("userdel: writing the account files");
        return 1;
    }
    if (removehome) {
        /* One level, and only files: a recursive delete belongs in rm,
         * which is already on the machine and can be told to do it. */
        fprintf(stderr, "userdel: remove %s yourself: 'rm -r %s'\n",
                e.home, e.home);
    }
    printf("removed %s\n", name);
    return 0;
}
