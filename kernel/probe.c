/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * probe.c - what the kernel says about the CPU and memory as it starts.
 *
 * Only the parts of the machine that have no driver: the processor
 * itself, its FPU, and how much RAM is fitted. Everything with a driver
 * reports itself as it registers, from main.c, because a device that
 * announces what it found is telling you something a list compiled at
 * build time cannot.
 */
#include "kernel.h"
#include "console.h"
#include "string.h"

static void status(const char *label)
{
    int n = (int)strlen(label);

    kputs("  ");
    kputs(label);
    while (n++ < 8) {
        kputc(' ');
    }
    kputs(": ");
}

static u32 get_vbr(void)
{
    u32 v;

    __asm__ volatile ("movec %%vbr,%0" : "=d"(v));
    return v;
}

static u16 get_sr(void)
{
    u16 v;

    __asm__ volatile ("move.w %%sr,%0" : "=d"(v));
    return v;
}

/*
 * Size RAM by testing each megabyte boundary until one does not answer.
 *
 * mem_probe() (memprobe.s) does the dangerous part: the first address
 * past the end of memory raises a bus error rather than simply reading
 * back wrong, so the probe has to survive the fault, and it installs its
 * own bus error handler for the duration.
 *
 * Two things are checked beyond "did the value stick". A sentinel in the
 * kernel's own data catches an address space that wraps -- which would
 * otherwise be reported as however much RAM the probe bothered to look
 * for, while the writes quietly landed on top of the kernel. And the
 * pattern differs per megabyte, so an alias mapping two boundaries to
 * the same memory does not look like two working ones.
 */
extern int mem_probe(volatile void *addr, u32 pattern);

static volatile u32 memory_sentinel;

u32 probe_memory(void)
{
    u32 mb;

    memory_sentinel = 0x5a5aa5a5;

    for (mb = 1; mb < 64; mb++) {
        if (!mem_probe((volatile void *)(mb * 0x100000UL),
                       0xc0de0000UL | mb)) {
            break;                      /* nothing answers here */
        }
        if (memory_sentinel != 0x5a5aa5a5) {
            memory_sentinel = 0x5a5aa5a5;
            break;                      /* the address space wraps */
        }
    }
    return mb * 0x100000UL;
}

/*
 * The 68040's FPU is on the die, so this is a check that it works rather
 * than a search for it. The value is computed at run time through a
 * volatile so the compiler cannot fold it away and report success for
 * arithmetic it did itself.
 */
static int probe_fpu(u32 *millionths)
{
    volatile double a = 1.0;
    volatile double b = 3.0;
    double r = a / b;

    *millionths = (u32)(r * 1000000.0);
    return (*millionths > 333332 && *millionths < 333334);
}

void probe_all(void)
{
    u32 v;

    status("cpu");
    kputs("MC68040, supervisor mode, sr=0x");
    kputhex16(get_sr());
    kputs(" vbr=0x");
    kputhex32(get_vbr());
    kputc('\n');

    status("fpu");
    if (probe_fpu(&v)) {
        kputs("on-chip, 1/3 = 0.");
        kputdec(v);
        kputc('\n');
    } else {
        kputs("NOT RESPONDING (1/3 came back as ");
        kputdec(v);
        kputs(" millionths)\n");
    }

    status("memory");
    v = probe_memory();
    kputdec(v / 1024);
    kputs(" KB, kernel 0x00000000-0x");
    kputhex32((u32)_end);
    kputs(", stack top 0x");
    kputhex32((u32)_stack_top);
    kputc('\n');
}
