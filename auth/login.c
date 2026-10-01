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
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <grp.h>
#include <signal.h>
#include <time.h>
#include <utmp.h>
#include <sys/time.h>
#include <sys/stat.h>
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
/*
 * A $6$ hash of a random string that was never written down, used when
 * the account does not exist or its password is locked.
 *
 * It is here so that THE CRYPT ALWAYS RUNS. Returning early for an
 * unknown name skips a full SHA-512 and answers in no time at all,
 * while a name that does exist takes as long as the hashing does --
 * which tells whoever is guessing which of the two they found, one
 * name per attempt. The fixed delay in fail() does not hide it: it is
 * added to both.
 *
 * Nothing anybody can type hashes to this, so a locked or absent
 * account still fails; it fails after the same work.
 */
static const char absent_hash[] =
    "$6$NoSuchAccount00$2Br9IP7f.vtq4DjWiHcS58UcpRfK1EJRpzUhEuB6oCFR"
    "/qO3micgdbdJaQEItMxb1nR/bSy5LgMXVVmQXDhsb1";

static int password_ok(const char *name, const char *pw)
{
    char stored[256];
    char *got;

    if (!name || sh_hash(name, stored, sizeof(stored)) != 0 ||
        stored[0] == '\0' || stored[0] == '*' || stored[0] == '!') {
        memcpy(stored, absent_hash, sizeof(absent_hash));
    }
    got = crypt(pw, stored);
    return got && strcmp(got, stored) == 0;
}

/*
 * THE RECORD OF A LOGIN, in /var/run/utmp (and /var/log/wtmp, if one is
 * kept): what who, w, uptime and top count. USER_PROCESS when the
 * session starts, DEAD_PROCESS when it ends -- which is why login stays
 * behind as the session's parent rather than becoming its shell. A
 * session with no terminal has no line to name and is not recorded.
 */
/* A utmp field: as much as fits, and NOT necessarily terminated --
 * the format's own rule, which readers honour with the field's size. */
static void fill(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);

    memcpy(dst, src, n < size ? n : size);
}

static void record(short type, pid_t pid, const char *user)
{
    struct utmp u;
    struct timeval tv;
    const char *line = ttyname(0), *host = getenv("SSH_CLIENT");
    size_t n, i;

    if (!line) {
        return;
    }
    if (strncmp(line, "/dev/", 5) == 0) {
        line += 5;
    }
    memset(&u, 0, sizeof(u));
    u.ut_type = type;
    u.ut_pid = pid;
    u.ut_session = pid;
    fill(u.ut_line, line, sizeof(u.ut_line));
    n = strlen(line);                   /* the id: the line's last four */
    for (i = 0; i < sizeof(u.ut_id) && i < n; i++) {
        u.ut_id[i] = line[n > 4 ? n - 4 + i : i];
    }
    if (type == USER_PROCESS) {
        fill(u.ut_user, user, sizeof(u.ut_user));
        if (host) {                     /* "client-ip port port" */
            for (i = 0; host[i] && host[i] != ' ' && i < sizeof(u.ut_host) - 1; i++) {
                u.ut_host[i] = host[i];
            }
        }
    }
    gettimeofday(&tv, 0);
    u.ut_tv.tv_sec = (int32_t)tv.tv_sec;
    u.ut_tv.tv_usec = (int32_t)tv.tv_usec;
    setutent();
    pututline(&u);
    endutent();
    updwtmp(_PATH_WTMP, &u);
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
            /* NOT `known && password_ok(...)`: && short-circuits, so an
             * unknown name skipped the crypt entirely and came back
             * fast. password_ok hashes against absent_hash when it has
             * no account, so both paths do the same work. */
            int good = preauth || password_ok(known ? name : 0, pw);
            int ok = known && good;

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
     * A LOGIN IS A SESSION, with the terminal as its controlling
     * terminal: that is what lets a hangup reach it and /dev/tty find
     * it. It forks, and the CHILD starts the session and takes the
     * terminal while it is still root (TIOCSCTTY's 1: take it even if
     * another session has it -- the console's shell, or the ssh
     * server's session that started this). The parent stays root and
     * waits, so that it can write the session's end into utmp, and so
     * that the shell or server that started it sees the session end.
     * (On the console this program is a process group leader, which
     * may not start a session itself; the fork is what makes it legal.)
     */
    {
        pid_t child = fork();

        if (child < 0) {
            perror("login: fork");
            return 1;
        }
        if (child > 0) {
            int st = 0;

            signal(SIGINT, SIG_IGN);
            signal(SIGQUIT, SIG_IGN);
            signal(SIGHUP, SIG_IGN);    /* the session's, not ours */
            record(USER_PROCESS, child, e.name);
            while (waitpid(child, &st, 0) < 0 && errno == EINTR) {
            }
            record(DEAD_PROCESS, child, e.name);
            /* The terminal back to root, for whoever logs in next. */
            if (isatty(0)) {
                (void)fchown(0, 0, 0);
                (void)fchmod(0, 0620);
            }
            return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        }
        signal(SIGHUP, SIG_DFL);
        if (setsid() < 0) {
            perror("login: setsid");
            return 1;
        }
        if (isatty(0) && ioctl(0, TIOCSCTTY, 1) != 0) {
            perror("login: TIOCSCTTY");
        }
        /*
         * THE TERMINAL BECOMES THE PERSON'S, while this is still root:
         * a terminal's mode is 0620 and enforced, so without this the
         * user's own programs could not reopen it (a full-screen editor
         * opening /dev/tty is the usual case), and anybody else's could.
         * The parent gives it back to root when the session ends.
         */
        if (isatty(0) && (fchown(0, e.uid, e.gid) != 0 || fchmod(0, 0620) != 0)) {
            perror("login: the terminal");
        }
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
