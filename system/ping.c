/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ping - is it there, and how far away?
 *
 *   ping ADDR [COUNT]
 *
 * No name resolution, so the address is numeric. That is the resolver
 * being absent rather than this program being simple.
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    u32 ip, rtt;
    int count = 4, sent = 0, ok = 0, i, err;

    if (argc < 2) {
        eputs("usage: ping HOST [COUNT]\n");
        return 1;
    }
    {
        struct in_addr a;

        if (resolve_host(argv[1], &a) < 0 || !a.s_addr) {
            eputs("ping: cannot resolve ");
            eputs(argv[1]);
            eputs("\n");
            return 1;
        }
        ip = a.s_addr;
    }
    if (argc > 2) {
        count = 0;
        for (i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++) {
            count = count * 10 + (argv[2][i] - '0');
        }
        if (count <= 0) {
            count = 4;
        }
    }

    for (i = 0; i < count; i++) {
        sent++;
        err = netctl(NETCTL_PING, ip, &rtt);
        if (err == 0) {
            ok++;
            puts("reply from ");
            put_ip(ip);
            puts(": seq=");
            putdec((u32)i + 1);
            puts(" time=");
            putdec(rtt);
            puts(" ms\n");
        } else {
            puts("no reply from ");
            put_ip(ip);
            puts(" (seq=");
            putdec((u32)i + 1);
            puts(")\n");
        }
    }

    puts("\n");
    putdec((u32)sent);
    puts(" sent, ");
    putdec((u32)ok);
    puts(" received, ");
    putdec((u32)((sent - ok) * 100 / sent));
    puts("% loss\n");

    /* The exit status is the answer, so a script can ask. */
    return ok ? 0 : 1;
}
