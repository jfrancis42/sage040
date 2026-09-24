/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * arp - show and change the ARP cache.
 *
 *   arp            show the cache
 *   arp -a         the same (accepted for habit)
 *   arp -d ADDR    forget one entry
 *
 * The cache maps an IP address to the hardware address a frame for it
 * goes to, learned by asking and remembered for a couple of minutes. It
 * is worth being able to drop an entry: a host whose MAC has changed --
 * a swapped card, a moved cable, a failed-over gateway -- answers at a
 * new address, and until the old entry ages out, frames go to a card
 * that is no longer there. `arp -d` is the fix, and this is also how you
 * prove the resolver re-learns rather than trusting it to.
 *
 * netstat -a shows the same cache; this is the tool that also EDITS it,
 * as arp(8) does. Addresses are numeric -- there is no name resolution.
 */
#include "ulib.h"

static void show(void)
{
    struct arpinfo a;
    int i, any = 0;

    for (i = 0; netctl(NETCTL_ARP, (u32)i, &a) == 0; i++) {
        any = 1;
        put_ip(a.ip);
        puts("  at ");
        put_mac(a.mac);
        puts("  (");
        putdec(a.age_ms / 1000);
        puts("s ago)\n");
    }
    if (!any) {
        puts("no entries\n");
    }
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 &&
        (strcmp(argv[1], "-a") == 0 || strcmp(argv[1], "-n") == 0))) {
        show();
        return 0;
    }

    if (argc == 3 && strcmp(argv[1], "-d") == 0) {
        struct in_addr a;
        int r;

        if (!inet_aton(argv[2], &a)) {
            eputs("arp: not an address\n");
            return 1;
        }
        r = netctl(NETCTL_ARPDEL, a.s_addr, 0);
        if (r < 0) {
            eputs(r == -ENOENT ? "arp: no such entry\n"
                               : "arp: delete failed\n");
            return 1;
        }
        return 0;
    }

    eputs("usage: arp [-a] | arp -d ADDR\n");
    return 2;
}
