/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fsmanifest DIR - everything under DIR, one line per name, for
 * comparing with the same listing made by Linux (kernel/linuxfstest.sh,
 * kernel/fsmanifest.py).
 *
 *   path  type  mode  uid  gid  ino  nlink  size  mtime  hash  target
 *
 * type is f, d, l or p; mode is the permission bits in octal (setuid,
 * setgid and sticky included); hash is FNV-1a over the file's bytes, 64
 * bits in hex, "-" for anything but a file; target is a symbolic link's,
 * "-" otherwise. The inode number and mtime are on the disk, so Linux
 * reading the same volume must report the same ones -- which makes a
 * hard link's identity, and a time this kernel wrote, part of the
 * comparison. Paths are relative to DIR and the lines are sorted, so
 * the order directories happen to list names in does not matter.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAXLINES 20000

static char *lines[MAXLINES];
static int nlines;

static uint64_t fnv(const char *path)
{
    static unsigned char buf[8192];
    uint64_t h = 0xcbf29ce484222325ULL;
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t i;

        for (i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 0x100000001b3ULL;
        }
    }
    close(fd);
    return h;
}

static void add(const char *rel, const char *full, const struct stat *st)
{
    char line[2400], target[512] = "-", hash[24] = "-";
    char type = S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' :
                S_ISFIFO(st->st_mode) ? 'p' : 'f';

    if (type == 'l') {
        ssize_t n = readlink(full, target, sizeof(target) - 1);

        target[n < 0 ? 0 : n] = '\0';
    }
    if (type == 'f') {
        uint64_t h = fnv(full);

        snprintf(hash, sizeof(hash), "%08lx%08lx", (unsigned long)(h >> 32),
                 (unsigned long)(h & 0xffffffffUL));
    }
    snprintf(line, sizeof(line), "%s\t%c\t%04lo\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%s\t%s",
             rel, type, (unsigned long)(st->st_mode & 07777),
             (unsigned long)st->st_uid, (unsigned long)st->st_gid,
             (unsigned long)st->st_ino, (unsigned long)st->st_nlink,
             type == 'd' ? 0UL : (unsigned long)st->st_size,
             (unsigned long)st->st_mtime, hash, target);
    if (nlines < MAXLINES) {
        lines[nlines++] = strdup(line);
    }
}

static void walk(const char *root, const char *rel)
{
    char full[1024], sub[1024];
    DIR *d;
    struct dirent *e;

    snprintf(full, sizeof(full), "%s%s%s", root, *rel ? "/" : "", rel);
    d = opendir(full);
    if (!d) {
        return;
    }
    while ((e = readdir(d))) {
        struct stat st;
        char path[2100];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            (!*rel && !strcmp(e->d_name, "lost+found"))) {
            continue;
        }
        snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(path, sizeof(path), "%s/%s", root, sub);
        if (lstat(path, &st) != 0) {
            continue;
        }
        add(sub, path, &st);
        if (S_ISDIR(st.st_mode)) {
            walk(root, sub);
        }
    }
    closedir(d);
}

static int cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
    int i;

    if (argc != 2) {
        fprintf(stderr, "usage: fsmanifest DIR\n");
        return 2;
    }
    walk(argv[1], "");
    qsort(lines, (size_t)nlines, sizeof(lines[0]), cmp);
    for (i = 0; i < nlines; i++) {
        puts(lines[i]);
    }
    return nlines >= MAXLINES;
}
