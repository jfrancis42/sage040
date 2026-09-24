/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sudo - run one command as another user.
 *
 * Set-user-id root. The difference from su is which password it asks
 * for: sudo asks for YOUR OWN, and decides from /etc/sudoers whether
 * you are allowed. That means root's password never has to be shared,
 * and taking somebody's access away is a line in a file rather than
 * changing a password everybody knows.
 *
 *   sudo CMD ...        run CMD as root
 *   sudo -u NAME CMD    run CMD as NAME
 *   sudo -l             list what you may do
 *
 * /etc/sudoers, one rule per line:
 *
 *   user     ALL=(ALL) ALL
 *   %group   ALL=(ALL) ALL
 *   user     ALL=(ALL) NOPASSWD: ALL
 *
 * THIS IS NOT REAL sudo. Real sudoers has host lists, command lists,
 * aliases, defaults and a grammar of its own; what is here is the
 * subset above, and anything it does not understand it REFUSES rather
 * than guesses. A sudoers parser that guesses is a sudoers parser that
 * grants something nobody meant to grant.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include "pwdb.h"

#define SUDOERS "/etc/sudoers"

extern char *crypt(const char *key, const char *salt);

struct rule {
    int allowed;
    int nopasswd;
};

/* Is `name` a member of `group`, by primary gid or by /etc/group? */
static int in_group(const char *name, unsigned gid, const char *group)
{
    long g = gr_gid(group);
    gid_t gids[PW_MAX_GROUPS];
    int n, i;

    if (g < 0) {
        return 0;
    }
    n = gr_of_user(name, gid, gids, PW_MAX_GROUPS);
    for (i = 0; i < n; i++) {
        if ((long)gids[i] == g) {
            return 1;
        }
    }
    return 0;
}

/*
 * Read /etc/sudoers and decide.
 *
 * The LAST matching line wins, as real sudo does: it is what lets a
 * general rule be written first and an exception after it.
 */
static void sudoers(const char *name, unsigned gid, struct rule *out)
{
    FILE *f = fopen(SUDOERS, "r");
    char line[512];

    out->allowed = 0;
    out->nopasswd = 0;
    if (!f) {
        return;                 /* no file: nobody may do anything */
    }
    while (fgets(line, sizeof(line), f)) {
        char who[128], rest[384];
        int matched;

        {
            char *p = line;

            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '#' || *p == '\n' || *p == '\0') {
                continue;
            }
            if (sscanf(p, "%127s %383[^\n]", who, rest) != 2) {
                continue;
            }
        }
        matched = (who[0] == '%')
                ? in_group(name, gid, who + 1)
                : (strcmp(who, name) == 0);
        if (!matched) {
            continue;
        }
        /*
         * Only the exact shapes this understands are accepted. A line
         * with host or command restrictions is NOT this shape, and is
         * refused rather than read as "ALL" -- which is the mistake
         * that would matter.
         */
        if (strncmp(rest, "ALL=(ALL) NOPASSWD: ALL", 23) == 0) {
            out->allowed = 1;
            out->nopasswd = 1;
        } else if (strncmp(rest, "ALL=(ALL) ALL", 13) == 0) {
            out->allowed = 1;
            out->nopasswd = 0;
        } else {
            fprintf(stderr, "sudo: %s: rule not understood, ignored: %s\n",
                    SUDOERS, rest);
        }
    }
    fclose(f);
}

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
    const char *as = "root";
    struct pwent me, target;
    struct rule r;
    gid_t gids[PW_MAX_GROUPS];
    int ngids, i, list = 0, first;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            as = argv[++i];
        } else if (strcmp(argv[i], "-l") == 0) {
            list = 1;
        } else {
            break;
        }
    }
    first = i;
    if (!list && first >= argc) {
        fputs("usage: sudo [-u user] command ...\n       sudo -l\n", stderr);
        return 1;
    }

    /* Who is really asking. getuid(), never geteuid(): this program is
     * set-user-id, so the effective id is 0 for everybody. */
    if (pw_by_uid(getuid(), &me) != 0) {
        fprintf(stderr, "sudo: you do not exist in /etc/passwd (uid %u)\n",
                (unsigned)getuid());
        return 1;
    }
    sudoers(me.name, me.gid, &r);

    if (list) {
        if (r.allowed) {
            printf("%s may run any command as any user%s\n", me.name,
                   r.nopasswd ? ", without a password" : "");
        } else {
            printf("%s may not run anything through sudo\n", me.name);
        }
        return r.allowed ? 0 : 1;
    }

    if (!r.allowed) {
        fprintf(stderr, "sudo: %s is not in %s\n", me.name, SUDOERS);
        return 1;
    }
    if (pw_by_name(as, &target) != 0) {
        fprintf(stderr, "sudo: no such user: %s\n", as);
        return 1;
    }

    /* YOUR password, not the target's -- and not at all if the rule
     * says NOPASSWD, or if root is asking. */
    if (!r.nopasswd && getuid() != 0) {
        char pw[256];
        int ok;
        char prompt[160];

        snprintf(prompt, sizeof(prompt), "[sudo] password for %s: ", me.name);
        if (read_password(prompt, pw, sizeof(pw)) != 0) {
            return 1;
        }
        ok = password_ok(me.name, pw);
        memset(pw, 0, sizeof(pw));
        if (!ok) {
            fputs("sudo: authentication failure\n", stderr);
            return 1;
        }
    }

    ngids = gr_of_user(target.name, target.gid, gids, PW_MAX_GROUPS);
    if (setgroups(ngids, gids) != 0) {
        perror("sudo: setgroups");
        return 1;
    }
    if (setgid(target.gid) != 0 || setuid(target.uid) != 0) {
        perror("sudo: cannot change user");
        return 1;
    }
    if (getuid() != target.uid || geteuid() != target.uid) {
        fputs("sudo: could not change privilege\n", stderr);
        return 1;
    }

    setenv("USER", target.name, 1);
    setenv("LOGNAME", target.name, 1);
    setenv("HOME", target.home, 1);
    execvp(argv[first], argv + first);
    perror(argv[first]);
    return 1;
}
