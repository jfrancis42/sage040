/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * irqs - interrupts taken, by MFP channel.
 *
 * Linux's /proc/interrupts, for a machine with one interrupt controller
 * and no /proc. Only the channels that something is connected to on
 * this board are named.
 */
#include "ulib.h"

static const char *names[16] = {
    0, "keyboard", "rtc", "ethernet", "timer", 0, "disk", "serial",
};

int main(void)
{
    struct irqstats s;
    int i;

    memset(&s, 0, sizeof(s));
    if (syscall(__NR_kstat, KSTAT_IRQ, sizeof(s), (u32)&s) < 0) {
        puts("irqs: no interrupt counts\n");
        return 1;
    }
    for (i = 0; i < 16; i++) {
        if (s.count[i] || (i < 8 && names[i])) {
            puts("  ");
            if (i < 10) {
                putch(' ');
            }
            putdec((u32)i);
            puts("  ");
            putdec(s.count[i]);
            putch(' ');
            puts(i < 8 && names[i] ? names[i] : "?");
            putch('\n');
        }
    }
    puts("  spurious ");
    putdec(s.spurious);
    puts(", input overruns ");
    putdec(s.tty_overruns);
    putch('\n');
    {
        struct kstackstats k;

        memset(&k, 0, sizeof(k));
        if (syscall(__NR_kstat, KSTAT_STACK, sizeof(k), (u32)&k) == 0) {
            puts("  kernel stack: ");
            putdec(k.max_used);
            puts(" of ");
            putdec(k.size);
            puts(" bytes at most, by ");
            puts(k.max_task[0] ? k.max_task : "?");
            putch('\n');
        }
    }
    puts("  disk waits: ");
    putdec(s.disk_slept);
    puts(" slept, ");
    putdec(s.disk_polled);
    puts(" polled\n");
    return 0;
}
