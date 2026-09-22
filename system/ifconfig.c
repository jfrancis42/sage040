/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * ifconfig - show or set the interface address.
 *
 * This was a shell builtin, and moving it out is not cosmetic. A
 * builtin runs inside the kernel with the kernel's privileges; this
 * runs unprivileged in an address space of its own and can do only what
 * the system call interface allows. Anything that CAN be a program
 * should be one, and the ones that cannot are the argument for a system
 * call that is missing.
 *
 *   ifconfig                       show it
 *   ifconfig ADDR MASK [GATEWAY]   set it
 *   ifconfig dhcp                  ask the network
 */
#include "ulib.h"

static void show(void)
{
    struct netinfo ni;

    if (netctl(NETCTL_INFO, 0, &ni) < 0) {
        eputs("ifconfig: no interface\n");
        exit(1);
    }

    puts(ni.name);
    puts("  hwaddr ");
    put_mac(ni.mac);
    puts(ni.up ? "  UP\n" : "  DOWN\n");

    puts("      inet ");
    put_ip(ni.ip);
    puts("  netmask ");
    put_ip(ni.netmask);
    puts("  gateway ");
    put_ip(ni.gateway);
    if (ni.dns) {
        puts("  dns ");
        put_ip(ni.dns);
    }
    puts("\n");

    puts("      RX ");
    putdec(ni.rx_packets);
    puts(" packets, ");
    putdec(ni.rx_dropped);
    puts(" dropped    TX ");
    putdec(ni.tx_packets);
    puts(" packets, ");
    putdec(ni.tx_errors);
    puts(" errors\n");
}

int main(int argc, char **argv)
{
    struct netaddr a;
    int i;

    if (argc == 1) {
        show();
        return 0;
    }

    if (strcmp(argv[1], "dhcp") == 0) {
        puts("requesting a lease...\n");
        if (netctl(NETCTL_DHCP, 0, &a) < 0) {
            eputs("ifconfig: no answer\n");
            return 1;
        }
        show();
        return 0;
    }

    if (argc < 3) {
        eputs("usage: ifconfig [ADDR MASK [GATEWAY]]\n");
        eputs("       ifconfig dhcp\n");
        return 1;
    }

    {
        struct in_addr ip, mask, gw;

        gw.s_addr = 0;
        if (!inet_aton(argv[1], &ip) || !inet_aton(argv[2], &mask) ||
            (argc > 3 && !inet_aton(argv[3], &gw))) {
            eputs("ifconfig: not an address\n");
            return 1;
        }
        a.ip = ip.s_addr;
        a.netmask = mask.s_addr;
        a.gateway = gw.s_addr;
    }
    if (!a.ip || !a.netmask) {
        eputs("ifconfig: not an address\n");
        return 1;
    }
    if ((i = netctl(NETCTL_SETADDR, 0, &a)) < 0) {
        eputs("ifconfig: cannot set it\n");
        return 1;
    }
    show();
    return 0;
}
