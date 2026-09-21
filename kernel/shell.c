/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * shell.c - a console shell.
 *
 * This is not a design goal in itself; it is how the console and
 * filesystem calls get exercised by hand.  Every command below is a
 * direct call into fs.c or console.c and nothing else, so if a command
 * misbehaves the fault is in the layer underneath rather than here.
 *
 * It runs in supervisor mode as part of the kernel.  When user programs
 * exist this becomes one of them, reaching the same calls through
 * TRAP #0 instead of directly -- which is the point of the gate already
 * being in place.
 */
#include "kernel.h"
#include "fs.h"
#include "block.h"
#include "string.h"

#define LINE_MAX   128
#define MAX_ARGS   8
#define COPY_CHUNK 512

static char line[LINE_MAX];
static char *argv[MAX_ARGS];
static u8 iobuf[COPY_CHUNK];

static void report(const char *what, int err)
{
    kputs(what);
    kputs(": ");
    kputs(fs_strerror(err));
    kputc('\n');
}

/* Split the line in place on runs of whitespace. */
static int split(char *s)
{
    int argc = 0;

    while (*s && argc < MAX_ARGS) {
        while (*s == ' ' || *s == '\t') {
            *s++ = '\0';
        }
        if (*s == '\0') {
            break;
        }
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') {
            s++;
        }
    }
    return argc;
}

/* ---------------------------------------------------------------- */

static void cmd_help(void)
{
    kputs("commands:\n"
          "  ls                  list the root directory\n"
          "  cat FILE            print a file\n"
          "  hd FILE             hex dump the first 256 bytes\n"
          "  write FILE          type a file in; a lone '.' ends it\n"
          "  append FILE         the same, added to the end\n"
          "  cp FROM TO          copy a file\n"
          "  mv FROM TO          rename a file\n"
          "  rm FILE             delete a file\n"
          "  stat FILE           size, attributes, first cluster\n"
          "  free                space used and available\n"
          "  echo TEXT           print a line\n"
          "  ver                 kernel version\n"
          "  halt                stop the machine\n");
}

static void print_attrs(u8 a)
{
    kputc((a & FS_ATTR_RDONLY)  ? 'r' : '-');
    kputc((a & FS_ATTR_HIDDEN)  ? 'h' : '-');
    kputc((a & FS_ATTR_SYSTEM)  ? 's' : '-');
    kputc((a & FS_ATTR_ARCHIVE) ? 'a' : '-');
    kputc((a & FS_ATTR_DIR)     ? 'd' : '-');
}

static void put2(u32 v)
{
    kputc((char)('0' + (v / 10) % 10));
    kputc((char)('0' + v % 10));
}

/* MS-DOS packs the date as year-1980 / month / day, and the time as
 * hour / minute / seconds-over-two. */
static void print_date(u16 date)
{
    kputdec(1980 + ((date >> 9) & 0x7f));
    kputc('-');
    put2((date >> 5) & 0x0f);
    kputc('-');
    put2(date & 0x1f);
}

static void print_time(u16 time)
{
    put2((time >> 11) & 0x1f);
    kputc(':');
    put2((time >> 5) & 0x3f);
}

static void cmd_ls(void)
{
    struct fs_dirent de;
    int i = 0;
    u32 bytes = 0;

    while (fs_readdir(i, &de) == FS_OK) {
        int n;

        print_attrs(de.attr);
        kputs("  ");
        kputs(de.name);
        for (n = (int)strlen(de.name); n < 14; n++) {
            kputc(' ');
        }
        kputdec(de.size);
        kputs("  ");
        print_date(de.date);
        kputc(' ');
        print_time(de.time);
        kputc('\n');

        bytes += de.size;
        i++;
    }
    kputdec((u32)i);
    kputs(i == 1 ? " file, " : " files, ");
    kputdec(bytes);
    kputs(" bytes\n");
}

static void cmd_cat(const char *name)
{
    int fd = fs_open(name, O_READ);
    s32 n;

    if (fd < 0) {
        report(name, fd);
        return;
    }
    while ((n = fs_read(fd, iobuf, sizeof(iobuf))) > 0) {
        s32 i;

        for (i = 0; i < n; i++) {
            kputc((char)iobuf[i]);
        }
    }
    if (n < 0) {
        report(name, (int)n);
    }
    fs_close(fd);
}

static void cmd_hexdump(const char *name)
{
    int fd = fs_open(name, O_READ);
    s32 n, i;
    u32 base = 0;

    if (fd < 0) {
        report(name, fd);
        return;
    }
    n = fs_read(fd, iobuf, 256);
    if (n < 0) {
        report(name, (int)n);
        fs_close(fd);
        return;
    }
    for (base = 0; (s32)base < n; base += 16) {
        kputhex32(base);
        kputs("  ");
        for (i = 0; i < 16; i++) {
            if ((s32)(base + i) < n) {
                kputhex8(iobuf[base + i]);
                kputc(' ');
            } else {
                kputs("   ");
            }
        }
        kputc('|');
        for (i = 0; i < 16 && (s32)(base + i) < n; i++) {
            u8 c = iobuf[base + i];

            kputc((c >= 32 && c < 127) ? (char)c : '.');
        }
        kputs("|\n");
    }
    fs_close(fd);
}

/* Read lines from the console into a file until a line holding just a
 * dot, which is how ed and MS-DOS COPY CON both end input. */
static void cmd_write(const char *name, int append)
{
    int flags = O_WRITE | O_CREATE | (append ? O_APPEND : O_TRUNC);
    int fd = fs_open(name, flags);
    char buf[LINE_MAX];
    u32 total = 0;

    if (fd < 0) {
        report(name, fd);
        return;
    }
    kputs("end with a single '.' on its own line\n");
    for (;;) {
        int len = kgets(buf, sizeof(buf));
        s32 n;

        if (len == 1 && buf[0] == '.') {
            break;
        }
        buf[len] = '\n';
        n = fs_write(fd, buf, (u32)len + 1);
        if (n < 0) {
            report(name, (int)n);
            break;
        }
        total += (u32)n;
    }
    fs_close(fd);
    kputdec(total);
    kputs(" bytes written\n");
}

static void cmd_copy(const char *from, const char *to)
{
    int in, out;
    s32 n;
    u32 total = 0;

    in = fs_open(from, O_READ);
    if (in < 0) {
        report(from, in);
        return;
    }
    out = fs_open(to, O_WRITE | O_CREATE | O_TRUNC);
    if (out < 0) {
        report(to, out);
        fs_close(in);
        return;
    }
    while ((n = fs_read(in, iobuf, sizeof(iobuf))) > 0) {
        s32 w = fs_write(out, iobuf, (u32)n);

        if (w < 0) {
            report(to, (int)w);
            break;
        }
        total += (u32)w;
        if (w != n) {
            report(to, FS_ENOSPC);
            break;
        }
    }
    if (n < 0) {
        report(from, (int)n);
    }
    fs_close(in);
    fs_close(out);
    kputdec(total);
    kputs(" bytes copied\n");
}

static void cmd_stat(const char *name)
{
    struct fs_dirent de;
    int err = fs_stat(name, &de);

    if (err != FS_OK) {
        report(name, err);
        return;
    }
    kputs(de.name);
    kputs("\n  size          ");
    kputdec(de.size);
    kputs(" bytes\n  first cluster ");
    kputdec(de.cluster);
    kputs("\n  attributes    ");
    print_attrs(de.attr);
    kputs(" (0x");
    kputhex8(de.attr);
    kputs(")\n  modified      ");
    print_date(de.date);
    kputc(' ');
    print_time(de.time);
    kputc('\n');
}

static void cmd_free(void)
{
    u32 total = fs_total_bytes();
    u32 avail = fs_free_bytes();

    kputs("volume ");
    kputs(fs_label()[0] ? fs_label() : "(unlabelled)");
    kputs(", cluster ");
    kputdec(fs_cluster_bytes());
    kputs(" bytes\n");
    kputdec(avail / 1024);
    kputs(" KB free of ");
    kputdec(total / 1024);
    kputs(" KB\n");
}

static void cmd_ver(void)
{
    kputs(KERNEL_NAME " kernel ");
    kputs(kernel_version);
    kputs(" built ");
    kputs(kernel_build);
    kputc('\n');
}

/* ---------------------------------------------------------------- */

static int need(int argc, int want, const char *usage)
{
    if (argc < want) {
        kputs("usage: ");
        kputln(usage);
        return 0;
    }
    return 1;
}

void shell(void)
{
    for (;;) {
        int argc;
        int err;

        kputs("sage> ");
        kgets(line, sizeof(line));
        argc = split(line);
        if (argc == 0) {
            continue;
        }

        if (stricmp(argv[0], "help") == 0 || stricmp(argv[0], "?") == 0) {
            cmd_help();

        } else if (stricmp(argv[0], "ls") == 0 ||
                   stricmp(argv[0], "dir") == 0) {
            cmd_ls();

        } else if (stricmp(argv[0], "cat") == 0 ||
                   stricmp(argv[0], "type") == 0) {
            if (need(argc, 2, "cat FILE")) {
                cmd_cat(argv[1]);
            }

        } else if (stricmp(argv[0], "hd") == 0) {
            if (need(argc, 2, "hd FILE")) {
                cmd_hexdump(argv[1]);
            }

        } else if (stricmp(argv[0], "write") == 0) {
            if (need(argc, 2, "write FILE")) {
                cmd_write(argv[1], 0);
            }

        } else if (stricmp(argv[0], "append") == 0) {
            if (need(argc, 2, "append FILE")) {
                cmd_write(argv[1], 1);
            }

        } else if (stricmp(argv[0], "cp") == 0 ||
                   stricmp(argv[0], "copy") == 0) {
            if (need(argc, 3, "cp FROM TO")) {
                cmd_copy(argv[1], argv[2]);
            }

        } else if (stricmp(argv[0], "mv") == 0 ||
                   stricmp(argv[0], "ren") == 0) {
            if (need(argc, 3, "mv FROM TO")) {
                err = fs_rename(argv[1], argv[2]);
                if (err != FS_OK) {
                    report(argv[1], err);
                }
            }

        } else if (stricmp(argv[0], "rm") == 0 ||
                   stricmp(argv[0], "del") == 0) {
            if (need(argc, 2, "rm FILE")) {
                err = fs_unlink(argv[1]);
                if (err != FS_OK) {
                    report(argv[1], err);
                }
            }

        } else if (stricmp(argv[0], "stat") == 0) {
            if (need(argc, 2, "stat FILE")) {
                cmd_stat(argv[1]);
            }

        } else if (stricmp(argv[0], "free") == 0) {
            cmd_free();

        } else if (stricmp(argv[0], "echo") == 0) {
            int i;

            for (i = 1; i < argc; i++) {
                if (i > 1) {
                    kputc(' ');
                }
                kputs(argv[i]);
            }
            kputc('\n');

        } else if (stricmp(argv[0], "ver") == 0) {
            cmd_ver();

        } else if (stricmp(argv[0], "halt") == 0) {
            kputs("halting.\n");
            halt();

        } else {
            kputs(argv[0]);
            kputs(": no such command (try 'help')\n");
        }
    }
}
