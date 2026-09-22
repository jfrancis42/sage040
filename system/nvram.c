/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * nvram - settings that outlive a reboot, in the M48T59's NVRAM.
 *
 *   nvram                 list them
 *   nvram KEY             print one; exit status 1 if it is not set
 *   nvram KEY=VALUE       set one
 *   nvram -d KEY          delete one
 *   nvram -c              clear them all
 *
 * /dev/nvram is 8176 bytes of battery-backed RAM; the clock is the last
 * sixteen, and the kernel keeps those out of it. The layout here is
 * this program's own, and simple on purpose, so that a person with a
 * hex dump can read it:
 *
 *   0  "SAGE"       magic, so a blank or foreign NVRAM reads as empty
 *   4  length       two bytes, big-endian: the text that follows
 *   6  text         "KEY=VALUE\n" lines
 *
 * `ifconfig nvram` configures the interface from net.ip, net.mask,
 * net.gw and net.dns -- or from DHCP if net=dhcp -- which is what lets
 * /etc/rc say one thing on every machine.
 */
#include "ulib.h"

#define NV_SIZE   8176
#define NV_TEXT   6
#define NV_MAX    (NV_SIZE - NV_TEXT)

static char text[NV_MAX + 1];
static u32 len;

static int load(void)
{
    u8 head[NV_TEXT];
    int fd = open("/dev/nvram", O_RDONLY);

    len = 0;
    text[0] = '\0';
    if (fd < 0) {
        return fd;
    }
    if (read(fd, head, NV_TEXT) == NV_TEXT && memcmp(head, "SAGE", 4) == 0) {
        len = ((u32)head[4] << 8) | head[5];
        if (len > NV_MAX || read(fd, text, len) != (s32)len) {
            len = 0;                    /* damaged: treat as empty */
        }
    }
    text[len] = '\0';
    close(fd);
    return 0;
}

static int save(void)
{
    u8 head[NV_TEXT] = { 'S', 'A', 'G', 'E', (u8)(len >> 8), (u8)len };
    int fd = open("/dev/nvram", O_WRONLY);
    int ok;

    if (fd < 0) {
        return fd;
    }
    ok = write(fd, head, NV_TEXT) == NV_TEXT &&
         (len == 0 || write(fd, text, len) == (s32)len);
    close(fd);
    return ok ? 0 : -EIO;
}

/* The line for KEY: its start, and the length of the whole line with
 * its newline. -1 if there is none. */
static int find(const char *key, u32 *start, u32 *linelen)
{
    u32 k = (u32)strlen(key), i = 0;

    while (i < len) {
        u32 e = i;

        while (e < len && text[e] != '\n') {
            e++;
        }
        if (e - i > k && memcmp(text + i, key, k) == 0 && text[i + k] == '=') {
            *start = i;
            *linelen = e - i + (e < len ? 1 : 0);
            return 0;
        }
        i = e + 1;
    }
    return -1;
}

static void delete(const char *key)
{
    u32 s, n;

    if (find(key, &s, &n) == 0) {
        u32 k;

        for (k = s; k + n < len; k++) {
            text[k] = text[k + n];      /* down: overlapping is safe */
        }
        len -= n;
        text[len] = '\0';
    }
}

int main(int argc, char **argv)
{
    u32 s, n, i;

    if (load() < 0) {
        eputs("nvram: no /dev/nvram\n");
        return 2;
    }
    if (argc == 1) {
        puts(text);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        len = 0;
        return save() < 0;
    }
    if (argc == 3 && strcmp(argv[1], "-d") == 0) {
        delete(argv[2]);
        return save() < 0;
    }
    if (argc != 2) {
        eputs("usage: nvram [KEY | KEY=VALUE | -d KEY | -c]\n");
        return 2;
    }

    for (i = 0; argv[1][i] && argv[1][i] != '='; i++) {
    }
    if (argv[1][i] == '=') {
        char key[64];
        u32 add = (u32)strlen(argv[1]) + 1;

        if (i == 0 || i >= sizeof(key)) {
            eputs("nvram: bad key\n");
            return 2;
        }
        memcpy(key, argv[1], i);
        key[i] = '\0';
        delete(key);
        if (len + add > NV_MAX) {
            eputs("nvram: full\n");
            return 1;
        }
        memcpy(text + len, argv[1], add - 1);
        text[len + add - 1] = '\n';
        len += add;
        text[len] = '\0';
        return save() < 0;
    }

    if (find(argv[1], &s, &n) < 0) {
        return 1;
    }
    {
        u32 k = (u32)strlen(argv[1]) + 1;       /* past "KEY=" */
        u32 v = n - k - (text[s + n - 1] == '\n' ? 1 : 0);

        write(1, text + s + k, v);
        putch('\n');
    }
    return 0;
}
