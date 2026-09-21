/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * netstat - what the network layer is holding.
 *
 * Connections and the address cache. The congestion window and the
 * measured round trip are shown because on this machine they are the
 * only way to see that the congestion control and the RTT estimator are
 * doing anything at all -- a stack without them looks identical from
 * outside until the moment a packet is lost.
 *
 *   netstat        connections
 *   netstat -a     connections and the ARP cache
 *   netstat -i     the interface
 */
#include "ulib.h"

static void connections(void)
{
    struct conninfo c;
    int i, any = 0;

    puts("Proto Local                Remote               State        "
         "Tx-Q Rx-Q  cwnd  rtt\n");
    for (i = 0; netctl(NETCTL_CONN, (u32)i, &c) == 0; i++) {
        int pad;

        any = 1;
        puts("tcp   ");

        put_ip(c.local_ip);
        putch(':');
        putdec(c.local_port);
        for (pad = 0; pad < 8; pad++) {
            putch(' ');
        }

        put_ip(c.remote_ip);
        putch(':');
        putdec(c.remote_port);
        for (pad = 0; pad < 8; pad++) {
            putch(' ');
        }

        puts(c.state_name);
        puts("  ");
        putdec(c.txq);
        puts("  ");
        putdec(c.rxq);
        puts("  ");
        putdec(c.cwnd);
        puts("  ");
        putdec(c.rtt_ms);
        puts("ms\n");
    }
    if (!any) {
        puts("no connections\n");
    }
}

static void arp_cache(void)
{
    struct arpinfo a;
    int i, any = 0;

    puts("\nAddress cache\n");
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

static void interface(void)
{
    struct netinfo ni;

    if (netctl(NETCTL_INFO, 0, &ni) < 0) {
        eputs("netstat: no interface\n");
        return;
    }
    puts(ni.name);
    puts("  inet ");
    put_ip(ni.ip);
    puts("  RX ");
    putdec(ni.rx_packets);
    puts("  TX ");
    putdec(ni.tx_packets);
    puts("  dropped ");
    putdec(ni.rx_dropped);
    puts("  errors ");
    putdec(ni.tx_errors);
    puts("\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-i") == 0) {
        interface();
        return 0;
    }

    connections();
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        arp_cache();
    }
    return 0;
}
