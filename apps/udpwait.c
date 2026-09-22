/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * udpwait - wait for one UDP datagram, and say what arrived.
 *
 *   udpwait ADDR PORT SECONDS
 *
 * Binds ADDR:PORT, waits up to SECONDS, and prints either
 *
 *   udpwait: from A.B.C.D: PAYLOAD
 *   udpwait: nothing
 *
 * lotest.sh sends frames from the host straight onto the emulated wire,
 * some of them addressed to 127.0.0.1, and this is how it finds out
 * which ones the stack let through.
 */
#include "ulib.h"

static int num(const char *s)
{
    int n = 0;

    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s++ - '0');
    }
    return n;
}

int main(int argc, char **argv)
{
    struct sockaddr_in a, from;
    struct in_addr ip;
    struct pollfd p;
    socklen_t flen = sizeof(from);
    char buf[128];
    int fd, r;

    if (argc != 4 || !inet_aton(argv[1], &ip)) {
        eputs("usage: udpwait ADDR PORT SECONDS\n");
        return 2;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)num(argv[2]));
    a.sin_addr = ip;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        eputs("udpwait: cannot bind\n");
        return 2;
    }
    puts("udpwait: listening\n");
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, num(argv[3]) * 1000) <= 0) {
        puts("udpwait: nothing\n");
        return 1;
    }
    r = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
    if (r < 0) {
        puts("udpwait: nothing\n");
        return 1;
    }
    buf[r] = '\0';
    puts("udpwait: from ");
    put_ip(from.sin_addr.s_addr);
    puts(": ");
    puts(buf);
    putch('\n');
    return 0;
}
