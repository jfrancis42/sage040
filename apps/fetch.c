/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fetch - ask a web server for a page and print what comes back.
 *
 * The point of this program is not the HTTP, which is one line. It is
 * that a TCP connection to a real server, over a real network, from a
 * hand-written stack, either works or does not -- and nothing short of
 * trying it proves anything. A three-way handshake, a request, a reply
 * arriving in several segments that have to be reassembled in order,
 * and an orderly close: every part of the state machine is exercised by
 * one fetch.
 *
 *   fetch HOST [PORT] [PATH]
 *
 * No DNS, so the address is numeric. That is the next thing missing
 * rather than an oversight -- a resolver is UDP and a packet format,
 * and it belongs above this.
 */
#include "ulib.h"

static void put_err(const char *what, int err)
{
    eputs(what);
    eputs(": error ");
    {
        char n[12];
        int i = 0, v = err < 0 ? -err : err;

        if (!v) {
            n[i++] = '0';
        }
        while (v > 0) {
            n[i++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (i > 0) {
            char c = n[--i];
            write(2, &c, 1);
        }
    }
    eputs("\n");
}

int main(int argc, char **argv)
{
    struct sockaddr_in sa;
    char req[256];
    char buf[512];
    const char *path = argc > 3 ? argv[3] : "/";
    int port = 80;
    int fd, i, n;
    u32 total = 0;
    s32 got;

    if (argc < 2) {
        eputs("usage: fetch HOST [PORT] [PATH]\n");
        return 1;
    }
    if (argc > 2) {
        port = 0;
        for (i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++) {
            port = port * 10 + (argv[2][i] - '0');
        }
        if (port <= 0 || port > 65535) {
            eputs("fetch: bad port\n");
            return 1;
        }
    }

    sa.sin_family = AF_INET;
    sa.sin_port = htons((u16)port);
    for (i = 0; i < 8; i++) {
        sa.sin_zero[i] = 0;
    }
    if (resolve_host(argv[1], &sa.sin_addr) < 0) {
        eputs("fetch: cannot resolve ");
        eputs(argv[1]);
        eputs("\n");
        return 1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        put_err("socket", fd);
        return 1;
    }

    puts("connecting to ");
    puts(argv[1]);
    puts("...\n");

    if ((i = connect(fd, (struct sockaddr *)&sa, sizeof(sa))) < 0) {
        put_err("connect", i);
        close(fd);
        return 1;
    }
    puts("connected\n");

    /* HTTP/1.0 so the server closes when it is done, which is how this
     * finds out the reply has ended without parsing Content-Length. */
    n = 0;
    {
        const char *p;

        for (p = "GET "; *p; p++) { req[n++] = *p; }
        for (p = path; *p; p++)   { req[n++] = *p; }
        for (p = " HTTP/1.0\r\nHost: "; *p; p++) { req[n++] = *p; }
        for (p = argv[1]; *p; p++) { req[n++] = *p; }
        for (p = "\r\nConnection: close\r\n\r\n"; *p; p++) { req[n++] = *p; }
    }

    if (write(fd, req, (u32)n) < 0) {
        put_err("write", -1);
        close(fd);
        return 1;
    }

    for (;;) {
        got = read(fd, buf, sizeof(buf));
        if (got == 0) {
            break;                      /* the server closed: done */
        }
        if (got < 0) {
            put_err("read", (int)got);
            break;
        }
        write(1, buf, (u32)got);
        total += (u32)got;
    }

    close(fd);
    puts("\n--- ");
    putdec(total);
    puts(" bytes ---\n");
    return 0;
}
