/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * host NAME - what address a name has.
 *
 * The resolver in lib/ulib (resolv.c): /etc/hosts, then DNS to the
 * servers in /etc/resolv.conf or the one DHCP gave. Prints the way
 * host(1) does, and exits 1 if the name does not resolve.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    struct in_addr a;
    int err;

    if (argc != 2) {
        eputs("usage: host NAME\n");
        return 2;
    }
    err = resolve_host(argv[1], &a);
    if (err < 0) {
        eputs("host: ");
        eputs(argv[1]);
        eputs(err == -ENOENT ? ": not found\n" :
              err == -ETIMEDOUT ? ": no answer from the name server\n" :
              err == -ENETUNREACH ? ": no name server configured\n" :
              ": lookup failed\n");
        return 1;
    }
    puts(argv[1]);
    puts(" has address ");
    puts(inet_ntoa(a));
    putch('\n');
    return 0;
}
