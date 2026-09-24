/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * su - become another user.
 *
 * Set-user-id root, so it starts with euid 0 whoever ran it, and the
 * real uid says who that was. Everything below is about giving that
 * privilege back correctly.
 *
 *   su          become root, asking for ROOT'S password
 *   su NAME     become NAME, asking for NAME'S password
 *   su -        as above, and make it a LOGIN shell: the home
 *               directory, the environment, the startup files
 *   su - NAME
 *   su NAME -c CMD
 *
 * Asking for the TARGET's password is what makes su different from
 * sudo, which asks for your own. It means su needs root's password
 * shared with everyone who may need it; sudo does not. Both are here
 * because they answer different questions.
 *
 * ROOT IS NOT ASKED FOR A PASSWORD, because root could simply become
 * the user another way; refusing would be theatre.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
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
    const char *target = "root";
    const char *command = 0;
    int login_shell = 0;
    struct pwent e;
    gid_t gids[PW_MAX_GROUPS];
    int ngids, i;
    char arg0[160];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0 ||
            strcmp(argv[i], "--login") == 0) {
            login_shell = 1;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (argv[i][0] == '-') {
            fputs("usage: su [-] [-c command] [user]\n", stderr);
            return 1;
        } else {
            target = argv[i];
        }
    }

    if (pw_by_name(target, &e) != 0) {
        fprintf(stderr, "su: no such user: %s\n", target);
        return 1;
    }

    /*
     * The password check, unless the caller is already root. getuid()
     * and not geteuid(): this program is set-user-id, so the effective
     * id is 0 for everybody and would let anybody through.
     */
    if (getuid() != 0) {
        char pw[256];
        int ok;

        if (read_password("Password: ", pw, sizeof(pw)) != 0) {
            return 1;
        }
        ok = password_ok(target, pw);
        memset(pw, 0, sizeof(pw));
        if (!ok) {
            fputs("su: authentication failure\n", stderr);
            return 1;
        }
    }

    /* Groups, group, user -- in that order, each needing the privilege
     * the one before it still has. */
    ngids = gr_of_user(e.name, e.gid, gids, PW_MAX_GROUPS);
    if (setgroups(ngids, gids) != 0) {
        perror("su: setgroups");
        return 1;
    }
    if (setgid(e.gid) != 0 || setuid(e.uid) != 0) {
        perror("su: cannot change user");
        return 1;
    }
    /*
     * AND IT MUST HAVE STUCK. setuid() can fail and be ignored, and
     * what is left is a root shell that believes it is somebody else.
     * This is the check that a set-user-id program exists to make.
     */
    if (getuid() != e.uid || geteuid() != e.uid) {
        fputs("su: could not drop privilege\n", stderr);
        return 1;
    }

    setenv("USER", e.name, 1);
    setenv("LOGNAME", e.name, 1);
    setenv("SHELL", e.shell, 1);
    if (login_shell) {
        setenv("HOME", e.home, 1);
        setenv("PATH", e.uid == 0 ? "/bin:/usr/bin:/sbin" : "/bin:/usr/bin", 1);
        if (chdir(e.home) != 0) {
            fprintf(stderr, "su: %s: %s\n", e.home, "cannot enter");
        }
    }

    if (command) {
        char *args[4];

        args[0] = (char *)e.shell;
        args[1] = (char *)"-c";
        args[2] = (char *)command;
        args[3] = 0;
        execv(e.shell, args);
        perror(e.shell);
        return 1;
    }

    /* A login shell is told so by a leading '-' in argv[0], which is
     * the only way it knows to read the startup files. */
    {
        const char *base = strrchr(e.shell, '/');
        char *args[2];

        base = base ? base + 1 : e.shell;
        snprintf(arg0, sizeof(arg0), "%s%s", login_shell ? "-" : "", base);
        args[0] = arg0;
        args[1] = 0;
        execv(e.shell, args);
    }
    perror(e.shell);
    return 1;
}
