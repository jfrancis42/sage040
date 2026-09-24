/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * useradd - make a new account.
 *
 * NOT set-user-id: only root may add a user, and root runs it as root.
 * A privileged program is a risk worth taking for passwd, which every
 * user needs; nobody but root ever needs this one.
 *
 *   useradd NAME                    uid chosen, /home/NAME, /bin/sh
 *   useradd -u N -g GID -s SHELL -d DIR -c "Real Name" NAME
 *   useradd -p PASSWORD NAME        set the password straight away
 *   useradd -G group,group NAME     add to supplementary groups
 *
 * It writes /etc/passwd, /etc/shadow and /etc/group, creates the home
 * directory and gives it to the new user. Without -p the account gets
 * '*' -- it exists and cannot be logged into until passwd(1) sets a
 * password, which is the right default: an account created with no
 * password at all is a way in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pwdb.h"

extern char *crypt(const char *key, const char *salt);

int main(int argc, char **argv)
{
    struct pwent e, exists;
    const char *name = 0, *shell = "/bin/sh", *home = 0, *gecos = "";
    const char *password = 0, *groups = 0;
    long uid = -1, gid = -1;
    char homebuf[128], salt[32];
    const char *hash = "*";
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            uid = strtol(argv[++i], 0, 10);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gid = strtol(argv[++i], 0, 10);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            shell = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            home = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            gecos = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            password = argv[++i];
        } else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) {
            groups = argv[++i];
        } else if (argv[i][0] == '-') {
            fputs("usage: useradd [-u uid] [-g gid] [-G groups] [-s shell]\n"
                  "               [-d home] [-c comment] [-p password] name\n",
                  stderr);
            return 1;
        } else {
            name = argv[i];
        }
    }
    if (!name) {
        fputs("useradd: no name given\n", stderr);
        return 1;
    }
    if (geteuid() != 0) {
        fputs("useradd: only root may add a user\n", stderr);
        return 1;
    }
    if (pw_by_name(name, &exists) == 0) {
        fprintf(stderr, "useradd: %s already exists\n", name);
        return 1;
    }
    /* A name with a colon in it would split the file into the wrong
     * fields and quietly corrupt every reader. */
    if (strchr(name, ':') || strchr(name, '\n') || name[0] == '\0') {
        fputs("useradd: a name may not contain ':' or a newline\n", stderr);
        return 1;
    }

    /* 1000 upward is the convention for people; below it is the
     * system's own. */
    e.uid = (uid >= 0) ? (unsigned)uid : pw_next_uid(1000);
    e.gid = (gid >= 0) ? (unsigned)gid : e.uid;
    if (!home) {
        snprintf(homebuf, sizeof(homebuf), "/home/%s", name);
        home = homebuf;
    }
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.gecos, sizeof(e.gecos), "%s", gecos);
    snprintf(e.home, sizeof(e.home), "%s", home);
    snprintf(e.shell, sizeof(e.shell), "%s", shell);

    if (password) {
        char *h;

        make_salt(salt, sizeof(salt));
        h = crypt(password, salt);
        if (!h) {
            fputs("useradd: could not hash the password\n", stderr);
            return 1;
        }
        hash = h;
    }

    if (pw_add(&e, hash) != 0) {
        perror("useradd: writing the account files");
        return 1;
    }

    /*
     * The home directory, owned by the new user. 0755 so that other
     * people can reach a file in it that the owner has made readable;
     * 0700 would be more private and is a decision for whoever runs
     * this, through chmod.
     */
    if (mkdir(e.home, 0755) != 0) {
        fprintf(stderr, "useradd: %s: could not create\n", e.home);
    } else if (chown(e.home, e.uid, e.gid) != 0) {
        fprintf(stderr, "useradd: %s: could not give it to %s\n",
                e.home, e.name);
    }

    if (groups) {
        fprintf(stderr, "useradd: -G is not implemented yet; add %s to "
                        "%s by hand in /etc/group\n", name, groups);
    }

    printf("added %s (uid %u, gid %u, home %s, shell %s)\n",
           e.name, e.uid, e.gid, e.home, e.shell);
    if (!password) {
        printf("no password set: run 'passwd %s' before it can log in\n",
               e.name);
    }
    return 0;
}
