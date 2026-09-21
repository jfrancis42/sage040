/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * httpd - serve files off the machine's own disk, over its own TCP.
 *
 * The other half of the network being real. fetch proves the machine can
 * open a connection; this proves something else can open one TO it,
 * which needs the listening side of the state machine, a passive open,
 * and an accept that hands back a second connection while the first
 * socket keeps listening.
 *
 * It also puts the two halves of the system together: the bytes come off
 * a FAT16 volume through the VFS, and go out through ethernet, IP and
 * TCP, and nothing in this program knows that either of those is
 * happening. It opens a file and writes it to a descriptor.
 *
 *   httpd [PORT]     default 80
 *
 * One connection at a time, served to completion before the next is
 * accepted -- there is no scheduler, so there is nothing to serve two
 * with. ctrl-C stops it.
 */
#include "ulib.h"

static const char not_found[] =
    "HTTP/1.0 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "\r\n"
    "no such file\n";

static void send_all(int fd, const char *p, u32 len)
{
    while (len > 0) {
        s32 n = write(fd, p, len);

        if (n <= 0) {
            return;
        }
        p += n;
        len -= (u32)n;
    }
}

/* The path out of "GET /what/ever HTTP/1.0", 8.3-ed for this volume. */
static int request_path(const char *req, char *out, int max)
{
    int i = 0, n = 0;

    while (req[i] && req[i] != ' ') {
        i++;                            /* the method */
    }
    while (req[i] == ' ') {
        i++;
    }
    if (req[i] != '/') {
        return -1;
    }
    i++;                                /* the leading slash */

    while (req[i] && req[i] != ' ' && req[i] != '\r' && req[i] != '\n' &&
           n < max - 1) {
        out[n++] = req[i++];
    }
    out[n] = '\0';
    return n;
}

int main(int argc, char **argv)
{
    struct sockaddr_in sa, peer;
    char req[512];
    char path[64];
    char buf[512];
    int port = 80, lfd, cfd, i;

    if (argc > 1) {
        port = 0;
        for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) {
            port = port * 10 + (argv[1][i] - '0');
        }
        if (port <= 0 || port > 65535) {
            eputs("httpd: bad port\n");
            return 1;
        }
    }

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        eputs("httpd: cannot make a socket\n");
        return 1;
    }

    sa.sin_family = AF_INET;
    sa.sin_port = htons((u16)port);
    sa.sin_addr = INADDR_ANY;
    for (i = 0; i < 8; i++) {
        sa.sin_zero[i] = 0;
    }

    if (bind(lfd, &sa) < 0 || listen(lfd, 4) < 0) {
        eputs("httpd: cannot listen\n");
        close(lfd);
        return 1;
    }

    puts("listening on port ");
    putdec((u32)port);
    puts(" -- ctrl-C to stop\n");

    for (;;) {
        s32 n;
        int ffd;

        cfd = accept(lfd, &peer);
        if (cfd < 0) {
            eputs("httpd: accept failed\n");
            break;
        }

        puts("connection from ");
        putdec((peer.sin_addr >> 24) & 0xff); putch('.');
        putdec((peer.sin_addr >> 16) & 0xff); putch('.');
        putdec((peer.sin_addr >> 8) & 0xff);  putch('.');
        putdec(peer.sin_addr & 0xff);
        puts("\n");

        n = read(cfd, req, sizeof(req) - 1);
        if (n <= 0) {
            close(cfd);
            continue;
        }
        req[n] = '\0';

        if (request_path(req, path, (int)sizeof(path)) <= 0) {
            /* Bare "/" asks for the index. */
            path[0] = 'I'; path[1] = 'N'; path[2] = 'D'; path[3] = 'E';
            path[4] = 'X'; path[5] = '.'; path[6] = 'T'; path[7] = 'X';
            path[8] = 'T'; path[9] = '\0';
        }

        puts("  -> ");
        puts(path);
        puts("\n");

        ffd = open(path, 0);
        if (ffd < 0) {
            send_all(cfd, not_found, strlen(not_found));
            close(cfd);
            continue;
        }

        send_all(cfd,
                 "HTTP/1.0 200 OK\r\n"
                 "Content-Type: text/plain\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 strlen("HTTP/1.0 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Connection: close\r\n"
                        "\r\n"));

        for (;;) {
            s32 got = read(ffd, buf, sizeof(buf));

            if (got <= 0) {
                break;
            }
            send_all(cfd, buf, (u32)got);
        }
        close(ffd);
        close(cfd);
        puts("  done\n");
    }

    close(lfd);
    return 0;
}
