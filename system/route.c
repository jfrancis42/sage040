/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * route - show and change the IPv4 routing table.
 *
 *   route                 show the table
 *   route add default gw ADDR
 *   route add -net NET netmask MASK [gw ADDR] [metric N]
 *   route add -host HOST [gw ADDR] [metric N]
 *   route del default
 *   route del -net NET netmask MASK
 *   route del -host HOST
 *
 * net-tools' route(8), near enough. The kernel keeps the table (route.c
 * over in the network stack); this reads it and hands it edits. There is
 * no name resolution -- every address is shown and typed numerically, so
 * -n is the only behaviour and it is not a flag.
 */
#include "ulib.h"

/* "a.b.c.d" from a host-order address, left-justified in a `w`-wide
 * field. No printf in ulib, so the padding is counted by hand. */
static void field_ip(u32 a, int w)
{
    char buf[16];
    int n = 0, i;
    u32 o[4];

    o[0] = (a >> 24) & 0xff; o[1] = (a >> 16) & 0xff;
    o[2] = (a >> 8) & 0xff;  o[3] = a & 0xff;
    for (i = 0; i < 4; i++) {
        u32 v = o[i];
        if (v >= 100) { buf[n++] = (char)('0' + v / 100); }
        if (v >= 10)  { buf[n++] = (char)('0' + v / 10 % 10); }
        buf[n++] = (char)('0' + v % 10);
        if (i < 3)    { buf[n++] = '.'; }
    }
    buf[n] = '\0';
    puts(buf);
    while (n++ < w) {
        puts(" ");
    }
}

static void field(const char *s, int w)
{
    int n = (int)strlen(s);

    puts(s);
    while (n++ < w) {
        puts(" ");
    }
}

/* u32 -> decimal, no printf. */
static void unum(u32 v, char *out)
{
    char t[16];
    int k = 0, j;

    if (!v) { t[k++] = '0'; }
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    for (j = 0; j < k; j++) { out[j] = t[k - 1 - j]; }
    out[j] = '\0';
}

/* decimal string -> u32; junk yields 0, which is a fine default metric. */
static u32 uatoi(const char *s)
{
    u32 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s++ - '0');
    }
    return v;
}

static void show(void)
{
    struct routeinfo r;
    int i;

    puts("Destination     Gateway         Genmask         Flags Metric Iface\n");
    for (i = 0; netctl(NETCTL_ROUTE, (u32)i, &r) == 0; i++) {
        char flags[8], n[16];
        int f = 0;

        /* 0.0.0.0/0 is the default route; name it, as route(8) does. */
        if (r.dest == 0 && r.mask == 0) {
            field("default", 16);
        } else {
            field_ip(r.dest, 16);
        }
        /* An on-link route has no gateway; route(8) shows 0.0.0.0. */
        field_ip(r.gateway, 16);
        field_ip(r.mask, 16);

        if (r.flags & RTF_UP)      { flags[f++] = 'U'; }
        if (r.flags & RTF_GATEWAY) { flags[f++] = 'G'; }
        if (r.flags & RTF_HOST)    { flags[f++] = 'H'; }
        flags[f] = '\0';
        field(flags, 6);

        unum(r.metric, n);
        field(n, 7);
        puts(r.iface);
        puts("\n");
    }
}

/* Parse address ARG into host order; returns 1 on success. */
static int addr(const char *s, u32 *out)
{
    struct in_addr a;

    if (!inet_aton(s, &a)) {
        return 0;
    }
    *out = a.s_addr;            /* host order, as ifconfig uses it */
    return 1;
}

static int edit(int argc, char **argv, int add)
{
    struct routeinfo r;
    int i;

    memset(&r, 0, sizeof(r));
    r.flags = RTF_STATIC;

    /* The target: default, -net NET, or -host HOST. */
    if (argc >= 3 && strcmp(argv[2], "default") == 0) {
        r.dest = 0; r.mask = 0;
        i = 3;
    } else if (argc >= 4 && strcmp(argv[2], "-net") == 0) {
        if (!addr(argv[3], &r.dest)) { eputs("route: bad network\n"); return 2; }
        i = 4;
    } else if (argc >= 4 && strcmp(argv[2], "-host") == 0) {
        if (!addr(argv[3], &r.dest)) { eputs("route: bad host\n"); return 2; }
        r.mask = 0xffffffffUL;
        i = 4;
    } else {
        eputs("usage: route add|del default|-net NET|-host HOST "
              "[netmask MASK] [gw ADDR] [metric N]\n");
        return 2;
    }

    for (; i < argc; i++) {
        if (strcmp(argv[i], "netmask") == 0 && i + 1 < argc) {
            if (!addr(argv[++i], &r.mask)) { eputs("route: bad netmask\n"); return 2; }
        } else if (strcmp(argv[i], "gw") == 0 && i + 1 < argc) {
            if (!addr(argv[++i], &r.gateway)) { eputs("route: bad gateway\n"); return 2; }
        } else if (strcmp(argv[i], "metric") == 0 && i + 1 < argc) {
            r.metric = uatoi(argv[++i]);
        } else {
            eputs("route: unexpected argument: ");
            eputs(argv[i]); eputs("\n");
            return 2;
        }
    }

    i = netctl(add ? NETCTL_ROUTEADD : NETCTL_ROUTEDEL, 0, &r);
    if (i < 0) {
        eputs(add ? "route: add failed" : "route: delete failed");
        /* The kernel's reason, in the words route(8) uses. */
        if (i == -EEXIST)     { eputs(": route already exists"); }
        else if (i == -ESRCH) { eputs(": no such route"); }
        else if (i == -ENOSPC){ eputs(": table full"); }
        eputs("\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "-n") == 0)) {
        show();
        return 0;
    }
    if (strcmp(argv[1], "add") == 0) {
        return edit(argc, argv, 1);
    }
    if (strcmp(argv[1], "del") == 0 || strcmp(argv[1], "delete") == 0) {
        return edit(argc, argv, 0);
    }
    eputs("usage: route [-n] | route add ... | route del ...\n");
    return 2;
}
