/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * id - who this process belongs to.
 *
 * The identity is real, and so is what it costs you: a task carries a
 * real, effective and saved user and group id, inherited across fork
 * and exec and changed by setuid() under POSIX's rules, and the
 * filesystem decides what each one may read and write. This note used
 * to say the opposite -- that nothing was enforced, because a FAT
 * directory entry had nowhere to record an owner. The disk is ext2
 * now, every inode has an owner and a mode, and `kernel/logintest.sh`
 * proves the enforcement by having a user refused root's 0600 file.
 *
 * The names come from /etc/passwd and /etc/group, read here rather
 * than through getpwuid() because this program is built against
 * lib/ulib and not the C library.
 */
#include "ulib.h"

/*
 * Field `want` of the line in `file` whose field `key_field` is `key`.
 * One small parser for both files: /etc/passwd and /etc/group have the
 * same shape for the two fields this needs.
 */
static int lookup(const char *file, int key_field, const char *key,
                  int want, char *out, u32 size)
{
    char line[256];
    int fd = open(file, O_RDONLY);
    u32 n = 0;
    int found = 0, done = 0;

    if (fd < 0) {
        return 0;
    }
    while (!done) {
        char c;
        int r = read(fd, &c, 1);

        if (r <= 0) {
            done = 1;
            if (n == 0) {
                break;
            }
            c = '\n';
        }
        if (c != '\n') {
            if (n + 1 < sizeof(line)) {
                line[n++] = c;
            }
            continue;
        }
        line[n] = '\0';
        n = 0;
        if (!line[0] || line[0] == '#') {
            continue;
        }
        {
            char *f[8];
            int nf = 1;
            u32 i;

            f[0] = line;
            for (i = 0; line[i] && nf < 8; i++) {
                if (line[i] == ':') {
                    line[i] = '\0';
                    f[nf++] = &line[i + 1];
                }
            }
            if (nf > key_field && nf > want &&
                strcmp(f[key_field], key) == 0) {
                u32 k = 0;

                while (f[want][k] && k + 1 < size) {
                    out[k] = f[want][k];
                    k++;
                }
                out[k] = '\0';
                found = 1;
                break;
            }
        }
    }
    close(fd);
    return found;
}

static void decstr(u32 v, char *out)
{
    char tmp[12];
    int n = 0, k = 0;

    if (!v) {
        tmp[n++] = '0';
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n) {
        out[k++] = tmp[--n];
    }
    out[k] = '\0';
}

/* "1000(jfrancis)" -- or just "1000" if nothing knows the name. */
static void put_id(u32 id, const char *file, int name_field)
{
    char num[12], name[64];

    decstr(id, num);
    puts(num);
    if (lookup(file, 2, num, name_field, name, sizeof(name)) && name[0]) {
        putch('(');
        puts(name);
        putch(')');
    }
}

int main(int argc, char **argv)
{
    u32 uid = (u32)syscall(__NR_getuid, 0, 0, 0);
    u32 euid = (u32)syscall(__NR_geteuid, 0, 0, 0);
    u32 gid = (u32)syscall(__NR_getgid, 0, 0, 0);
    u32 egid = (u32)syscall(__NR_getegid, 0, 0, 0);
    char num[12];
    int only_u = 0, only_g = 0, names = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            only_u = 1;
        } else if (strcmp(argv[i], "-g") == 0) {
            only_g = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            names = 1;
        } else {
            eputs("usage: id [-u] [-g] [-n]\n");
            return 1;
        }
    }

    if (only_u || only_g) {
        u32 v = only_u ? uid : gid;

        if (names) {
            char name[64];

            decstr(v, num);
            if (lookup(only_u ? "/etc/passwd" : "/etc/group", 2, num, 0,
                       name, sizeof(name))) {
                puts(name);
                putch('\n');
                return 0;
            }
        }
        decstr(v, num);
        puts(num);
        putch('\n');
        return 0;
    }

    puts("uid=");
    put_id(uid, "/etc/passwd", 0);
    puts(" gid=");
    put_id(gid, "/etc/group", 0);
    if (euid != uid) {
        puts(" euid=");
        put_id(euid, "/etc/passwd", 0);
    }
    if (egid != gid) {
        puts(" egid=");
        put_id(egid, "/etc/group", 0);
    }
    putch('\n');
    return 0;
}
