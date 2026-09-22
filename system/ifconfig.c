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
 *   ifconfig nvram                 as the NVRAM says: net=dhcp, or
 *                                  net.ip, net.mask and net.gw (see
 *                                  /bin/nvram) -- so /etc/rc can say
 *                                  one thing on every machine
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

/* One NVRAM setting into `out`, or 0 if it is not set. The layout is
 * /bin/nvram's: "SAGE", a two-byte length, "KEY=VALUE" lines. */
static int nv_get(const char *key, char *out, u32 max)
{
    static char text[8176];
    u8 head[6];
    u32 len = 0, k = (u32)strlen(key), i = 0;
    int fd = open("/dev/nvram", O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    if (read(fd, head, 6) == 6 && memcmp(head, "SAGE", 4) == 0) {
        len = ((u32)head[4] << 8) | head[5];
        if (len >= sizeof(text) || read(fd, text, len) != (s32)len) {
            len = 0;
        }
    }
    close(fd);
    while (i < len) {
        u32 e = i;

        while (e < len && text[e] != '\n') {
            e++;
        }
        if (e - i > k && memcmp(text + i, key, k) == 0 && text[i + k] == '=' &&
            e - i - k - 1 < max) {
            memcpy(out, text + i + k + 1, e - i - k - 1);
            out[e - i - k - 1] = '\0';
            return 1;
        }
        i = e + 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct netaddr a;
    char nv_ip[20], nv_mask[20], nv_gw[20], nv_mode[20];
    char *fake[5];
    int i;

    if (argc == 2 && strcmp(argv[1], "nvram") == 0) {
        if (nv_get("net", nv_mode, sizeof(nv_mode)) &&
            strcmp(nv_mode, "dhcp") == 0) {
            argv[1] = "dhcp";
        } else if (nv_get("net.ip", nv_ip, sizeof(nv_ip)) &&
                   nv_get("net.mask", nv_mask, sizeof(nv_mask))) {
            fake[0] = argv[0];
            fake[1] = nv_ip;
            fake[2] = nv_mask;
            fake[3] = nv_get("net.gw", nv_gw, sizeof(nv_gw)) ? nv_gw : 0;
            fake[4] = 0;
            argc = fake[3] ? 4 : 3;
            argv = fake;
        } else {
            eputs("ifconfig: the NVRAM has no net settings\n");
            return 1;
        }
    }

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
