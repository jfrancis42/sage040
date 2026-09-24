/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * passwd - change a password.
 *
 * Set-user-id root, because /etc/shadow is 0600 root and an ordinary
 * user must still be able to change their own entry. That is the
 * narrowest possible reason for a program to be privileged, and the
 * whole of what this one does with it.
 *
 *   passwd          change your own
 *   passwd NAME     change NAME's (root only)
 *   passwd -l NAME  lock: no password will match (root only)
 *   passwd -d NAME  delete: the account gets '*' (root only)
 *
 * Changing your OWN password asks for the old one first. Root is not
 * asked: root can rewrite the file directly, so asking would be
 * theatre -- and root changing a forgotten password is the usual
 * reason to be here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pwdb.h"

extern char *crypt(const char *key, const char *salt);

static int password_ok(const char *name, const char *pw)
{
    char stored[256];
    char *got;

    if (sh_hash(name, stored, sizeof(stored)) != 0) {
        return 0;
    }
    if (stored[0] == '\0' || stored[0] == '*' || stored[0] == '!') {
        return 0;
    }
    got = crypt(pw, stored);
    return got && strcmp(got, stored) == 0;
}

int main(int argc, char **argv)
{
    struct pwent me, target;
    const char *name = 0;
    int lock = 0, del = 0, i;
    char salt[32], new1[256], new2[256];
    char *hash;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            lock = 1;
        } else if (strcmp(argv[i], "-d") == 0) {
            del = 1;
        } else if (argv[i][0] == '-') {
            fputs("usage: passwd [-l|-d] [name]\n", stderr);
            return 1;
        } else {
            name = argv[i];
        }
    }

    if (pw_by_uid(getuid(), &me) != 0) {
        fprintf(stderr, "passwd: you do not exist in /etc/passwd\n");
        return 1;
    }
    if (!name) {
        name = me.name;
    }
    if (pw_by_name(name, &target) != 0) {
        fprintf(stderr, "passwd: no such user: %s\n", name);
        return 1;
    }

    /* getuid(), not geteuid(): this is set-user-id and every caller's
     * effective id is 0. */
    if (getuid() != 0 && strcmp(name, me.name) != 0) {
        fprintf(stderr, "passwd: only root may change another user's "
                        "password\n");
        return 1;
    }
    if ((lock || del) && getuid() != 0) {
        fprintf(stderr, "passwd: only root may lock or clear a password\n");
        return 1;
    }

    if (lock || del) {
        if (sh_set_hash(name, lock ? "!" : "*") != 0) {
            perror("passwd: /etc/shadow");
            return 1;
        }
        printf("password %s for %s\n", lock ? "locked" : "cleared", name);
        return 0;
    }

    if (getuid() != 0) {
        char old[256];
        int ok;

        if (read_password("Current password: ", old, sizeof(old)) != 0) {
            return 1;
        }
        ok = password_ok(name, old);
        memset(old, 0, sizeof(old));
        if (!ok) {
            fputs("passwd: authentication failure\n", stderr);
            return 1;
        }
    }

    if (read_password("New password: ", new1, sizeof(new1)) != 0 ||
        read_password("Retype new password: ", new2, sizeof(new2)) != 0) {
        return 1;
    }
    if (strcmp(new1, new2) != 0) {
        fputs("passwd: they do not match\n", stderr);
        memset(new1, 0, sizeof(new1));
        memset(new2, 0, sizeof(new2));
        return 1;
    }
    if (new1[0] == '\0') {
        fputs("passwd: an empty password would let anybody in; refused\n",
              stderr);
        return 1;
    }

    /* A NEW SALT EVERY TIME. Reusing the old one would mean the same
     * password always hashed to the same string, and two accounts with
     * the same password would be visibly the same in the file. */
    make_salt(salt, sizeof(salt));
    hash = crypt(new1, salt);
    memset(new1, 0, sizeof(new1));
    memset(new2, 0, sizeof(new2));
    if (!hash) {
        fputs("passwd: could not hash the password\n", stderr);
        return 1;
    }
    if (sh_set_hash(name, hash) != 0) {
        perror("passwd: /etc/shadow");
        return 1;
    }
    printf("password changed for %s\n", name);
    return 0;
}
