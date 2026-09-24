/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * login - ask who you are, and become them.
 *
 * Started by /etc/rc on the console, and by dropbear for a session it
 * has already authenticated. It is the only program that turns a name
 * and a password into a running shell, and everything it does after
 * the password is checked is what makes a session a session:
 *
 *   - the supplementary groups, from /etc/group, BEFORE dropping
 *     privilege, because setgroups(2) is root's to call;
 *   - the group id, then the user id, IN THAT ORDER -- setuid first
 *     and the setgid that follows is refused, leaving a process with a
 *     user's uid and root's gid, which is a hole rather than a bug;
 *   - the working directory, to the home directory;
 *   - the environment: HOME, SHELL, USER, LOGNAME, PATH;
 *   - and then exec the shell named in /etc/passwd, with argv[0]
 *     prefixed by '-' so it knows it is a LOGIN shell and reads the
 *     startup files.
 *
 * WRONG PASSWORDS ARE SLOW AND VAGUE ON PURPOSE. The message is the
 * same whether the account exists or not -- "Login incorrect" -- so it
 * cannot be used to find out which names are real, and there is a
 * pause afterwards so that guessing is tedious.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <time.h>
#include "pwdb.h"

extern char *crypt(const char *key, const char *salt);

static void fail(void)
{
    struct timespec t;

    fputs("Login incorrect\n\n", stdout);
    fflush(stdout);
    t.tv_sec = 2;
    t.tv_nsec = 0;
    nanosleep(&t, 0);
}

/*
 * Does `pw` open the account?
 *
 * A '*' or '!' hash matches nothing: crypt never produces one, so the
 * comparison simply fails, but it is refused explicitly here so that
 * the intent is on the page.
 *
 * The comparison is against the stored hash used AS THE SALT, which is
 * what makes one function do both hashing and checking: the stored
 * string carries its own scheme, salt and rounds.
 */
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
    char name[PW_MAX_NAME], pw[256];
    struct pwent e;
    gid_t gids[PW_MAX_GROUPS];
    int ngids;
    const char *preauth = 0;
    char arg0[sizeof(e.shell) + 2];
    int i;

    /*
     * -f NAME: the caller has already established who this is, and
     * there is no password to ask for. dropbear uses it after a public
     * key has been accepted. Only root may say it -- otherwise it is a
     * way for anybody to become anybody.
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            preauth = argv[++i];
        } else {
            fputs("usage: login [-f name]\n", stderr);
            return 1;
        }
    }
    if (preauth && geteuid() != 0) {
        fputs("login: -f is root's\n", stderr);
        return 1;
    }

    for (;;) {
        if (preauth) {
            snprintf(name, sizeof(name), "%s", preauth);
        } else {
            fputs("\nlogin: ", stdout);
            fflush(stdout);
            if (!fgets(name, sizeof(name), stdin)) {
                return 1;
            }
            name[strcspn(name, "\r\n")] = '\0';
            if (name[0] == '\0') {
                continue;
            }
            if (read_password("Password: ", pw, sizeof(pw)) != 0) {
                return 1;
            }
        }

        /*
         * The account is looked up and the password checked even when
         * one of them is already known to be wrong, so that a name
         * that does not exist takes the same time as one that does.
         */
        {
            int known = (pw_by_name(name, &e) == 0);
            int ok = known && (preauth || password_ok(name, pw));

            memset(pw, 0, sizeof(pw));
            if (!ok) {
                if (preauth) {
                    fputs("login: no such account\n", stderr);
                    return 1;
                }
                fail();
                continue;
            }
        }
        break;
    }

    /*
     * PRIVILEGE GOES DOWN IN THIS ORDER AND NO OTHER: groups, then
     * group, then user. Each needs the privilege the previous one
     * still has.
     */
    ngids = gr_of_user(e.name, e.gid, gids, PW_MAX_GROUPS);
    if (setgroups(ngids, gids) != 0 && geteuid() == 0) {
        perror("login: setgroups");
        return 1;
    }
    if (setgid(e.gid) != 0) {
        perror("login: setgid");
        return 1;
    }
    if (setuid(e.uid) != 0) {
        perror("login: setuid");
        return 1;
    }
    /* And it must have WORKED. A setuid that silently did nothing
     * would leave a root shell wearing somebody's name. */
    if (getuid() != e.uid || geteuid() != e.uid) {
        fputs("login: could not drop privilege\n", stderr);
        return 1;
    }

    if (chdir(e.home) != 0) {
        fprintf(stderr, "login: %s: cannot enter, using /\n", e.home);
        if (chdir("/") != 0) {
            return 1;
        }
    }

    setenv("HOME", e.home, 1);
    setenv("SHELL", e.shell, 1);
    setenv("USER", e.name, 1);
    setenv("LOGNAME", e.name, 1);
    setenv("PATH", e.uid == 0 ? "/bin:/usr/bin:/sbin" : "/bin:/usr/bin", 1);

    /*
     * argv[0] BEGINS WITH '-'. That is the whole convention by which a
     * shell knows it was started by login and should read the login
     * startup files; it is not cosmetic, and without it .profile never
     * runs.
     */
    snprintf(arg0, sizeof(arg0), "-%s", e.shell);
    {
        char *base = strrchr(arg0, '/');

        if (base) {
            memmove(arg0 + 1, base + 1, strlen(base + 1) + 1);
        }
    }
    {
        char *args[2];

        args[0] = arg0;
        args[1] = 0;
        execv(e.shell, args);
    }
    perror(e.shell);
    return 1;
}
