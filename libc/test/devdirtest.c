/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * devdirtest - /dev as a directory, /dev/fd, and /dev/stdin and friends.
 *
 * A listing is checked against the thing it lists: every character
 * device readdir names must stat as one, and a pty made during the test
 * must appear in /dev/pts under the name ptsname() gives it. What
 * /dev/fd/N opens is checked by what comes through it.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    checks++;
    if (!ok) {
        failures++;
    }
}

/* The d_type readdir gives `name` in `dir`: -1 if it is not listed. */
static int listed(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int t = -1;

    while (d && (e = readdir(d))) {
        if (strcmp(e->d_name, name) == 0) {
            t = e->d_type;
        }
    }
    if (d) {
        closedir(d);
    }
    return t;
}

static int link_is(const char *path, const char *want)
{
    char buf[128];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);

    if (n < 0) {
        return 0;
    }
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

int main(void)
{
    struct stat st, st2;
    char path[300], buf[64], cwd[64];
    DIR *d;
    struct dirent *e;
    int fd, p[2], n, devs = 0, chars = 0, m;

    check("/dev is a directory", stat("/dev", &st) == 0 && S_ISDIR(st.st_mode));
    check("  and lists the devices, as character devices",
          listed("/dev", "null") == DT_CHR && listed("/dev", "zero") == DT_CHR &&
          listed("/dev", "console") == DT_CHR && listed("/dev", "ptmx") == DT_CHR);
    check("  pts and shm as directories, fd and stdin as links",
          listed("/dev", "pts") == DT_DIR && listed("/dev", "shm") == DT_DIR &&
          listed("/dev", "fd") == DT_LNK && listed("/dev", "stdin") == DT_LNK &&
          listed("/dev", ".") == DT_DIR);
    d = opendir("/dev");
    while (d && (e = readdir(d))) {
        if (e->d_type == DT_CHR) {
            devs++;
            snprintf(path, sizeof(path), "/dev/%s", e->d_name);
            if (stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
                chars++;
            } else {
                printf("         (%s does not stat as a device)\n", path);
            }
        }
    }
    if (d) {
        closedir(d);
    }
    check("  every device it lists stats as a character device",
          devs > 5 && chars == devs);

    check("/dev/fd is a link to /proc/self/fd",
          lstat("/dev/fd", &st) == 0 && S_ISLNK(st.st_mode) &&
          link_is("/dev/fd", "/proc/self/fd"));
    check("  /dev/stdin, stdout and stderr to /proc/self/fd/0, 1, 2",
          link_is("/dev/stdin", "/proc/self/fd/0") &&
          link_is("/dev/stdout", "/proc/self/fd/1") &&
          link_is("/dev/stderr", "/proc/self/fd/2"));

    pipe(p);
    snprintf(path, sizeof(path), "/dev/fd/%d", p[0]);
    fd = open(path, O_RDONLY);
    write(p[1], "through", 7);
    memset(buf, 0, sizeof(buf));
    check("opening /dev/fd/N of a pipe reads what goes into that pipe",
          fd >= 0 && read(fd, buf, sizeof(buf)) == 7 &&
          strcmp(buf, "through") == 0);
    snprintf(path, sizeof(path), "%d", p[1]);
    check("  and /dev/fd lists the descriptor", listed("/dev/fd", path) >= 0);
    close(fd);
    close(p[0]);
    close(p[1]);

    fstat(0, &st);
    fd = open("/dev/stdin", O_RDONLY);
    check("/dev/stdin opens what descriptor 0 is",
          fd >= 0 && fstat(fd, &st2) == 0 && st2.st_ino == st.st_ino &&
          st2.st_mode == st.st_mode);
    close(fd);

    fd = open("/tmp/devfd.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "abcdef", 6);
    snprintf(path, sizeof(path), "/dev/fd/%d", fd);
    n = open(path, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    check("/dev/fd/N of a file opens the file afresh, from the start",
          n >= 0 && read(n, buf, 3) == 3 && strcmp(buf, "abc") == 0 &&
          lseek(fd, 0, SEEK_CUR) == 6);
    close(n);
    close(fd);
    unlink("/tmp/devfd.txt");

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m >= 0) {
        grantpt(m);
        unlockpt(m);
    }
    {
        const char *name = m >= 0 ? ptsname(m) : 0;

        check("/dev/pts lists a pty made just now, under its ptsname",
              name && strncmp(name, "/dev/pts/", 9) == 0 &&
              listed("/dev/pts", name + 9) == DT_CHR);
    }
    if (m >= 0) {
        close(m);
    }

    check("chdir /dev, and a relative name there is the device",
          chdir("/dev") == 0 && getcwd(cwd, sizeof(cwd)) &&
          strcmp(cwd, "/dev") == 0 && (fd = open("null", O_WRONLY)) >= 0 &&
          write(fd, "x", 1) == 1 && close(fd) == 0);
    check("  and /dev/shm is still tmpfs",
          (fd = open("/dev/shm/x", O_CREAT | O_RDWR, 0600)) >= 0 &&
          close(fd) == 0 && unlink("/dev/shm/x") == 0);
    chdir("/");
    check("a name under /dev that is not there is ENOENT",
          open("/dev/nothere", O_RDONLY) == -1 && errno == ENOENT);

    printf("devdirtest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
