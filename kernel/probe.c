/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * probe.c - what the kernel says about the machine as it comes up.
 *
 * Each line is a device the kernel actually touched, not a list compiled
 * at build time.  Startup messages that report what was assumed rather
 * than what was found are worse than none, because they agree with you
 * while the hardware disagrees.
 */
#include "kernel.h"
#include "block.h"
#include "fs.h"
#include "string.h"

/* "  label   : " with the labels lined up. */
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
 * Two things are checked beyond "did the value stick".  A sentinel in
 * the kernel's own data catches an address space that wraps -- which
 * would otherwise be reported as however much RAM the probe bothered to
 * look for, while the writes quietly landed on top of the kernel.  And
 * the pattern differs per megabyte, so an alias that maps two boundaries
 * to the same memory does not look like two working ones.
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
 * than a search for it.  The value is computed at run time through a
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

static int probe_uart(void)
{
    u8 saved = MMIO8(UART_SCR);
    int ok;

    /* The scratch register is the standard way to tell a 16550 is
     * actually there: it is the one register with no side effects. */
    MMIO8(UART_SCR) = 0xa5;
    ok = (MMIO8(UART_SCR) == 0xa5);
    MMIO8(UART_SCR) = 0x5a;
    ok = ok && (MMIO8(UART_SCR) == 0x5a);
    MMIO8(UART_SCR) = saved;
    return ok;
}

static int probe_mfp(void)
{
    u8 saved = MMIO8(MFP_VR);
    int ok;

    MMIO8(MFP_VR) = 0x40;
    ok = ((MMIO8(MFP_VR) & 0xf8) == 0x40);
    MMIO8(MFP_VR) = saved;
    return ok;
}

void probe_all(void)
{
    u32 v;
    int i;

    /* --- CPU ------------------------------------------------------ */
    status("cpu");
    kputs("MC68040, supervisor mode, sr=0x");
    kputhex16(get_sr());
    kputs(" vbr=0x");
    kputhex32(get_vbr());
    kputc('\n');

    /* --- FPU ------------------------------------------------------ */
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

    /* --- memory --------------------------------------------------- */
    status("memory");
    v = probe_memory();
    kputdec(v / 1024);
    kputs(" KB, kernel 0x00000000-0x");
    kputhex32((u32)_end);
    kputs(", stack top 0x");
    kputhex32((u32)_stack_top);
    kputc('\n');

    /* --- console -------------------------------------------------- */
    status("console");
    kputs("NS16550A at 0xff000000, 8N1, ");
    kputs(probe_uart() ? "scratch register verified\n"
                       : "SCRATCH REGISTER FAILED\n");

    /* --- MFP ------------------------------------------------------ */
    status("mfp");
    if (probe_mfp()) {
        kputs("MC68901 at 0xff300000, 16 vectored channels on IPL ");
        kputdec(MFP_IPL);
        kputc('\n');
    } else {
        kputs("NOT FOUND at 0xff300000\n");
    }

    /* --- disk ----------------------------------------------------- */
    status("disk");
    if (blk_init() == 0) {
        u32 sectors = blk_capacity();

        kputs("ATA, '");
        kputs(blk_model());
        kputs("', ");
        kputdec(sectors);
        kputs(" sectors (");
        kputdec(sectors / 2048);
        kputs(" MiB)\n");
    } else {
        kputs("no drive responding at 0xff100000\n");
    }

    /* --- network -------------------------------------------------- */
    status("network");
    MMIO16(SMC_BANKSEL) = 3;
    v = MMIO16(SMC_B3_REV);
    if (v == 0x3391) {
        kputs("LAN91C111 rev 0x");
        kputhex16((u16)v);
        kputs(", MAC ");
        MMIO16(SMC_BANKSEL) = 1;
        for (i = 0; i < 6; i++) {
            kputhex8(MMIO8(SMC_B1_IA0 + i));
            if (i < 5) {
                kputc(':');
            }
        }
        kputs(" (set by the emulator; no driver has claimed the chip)\n");
    } else {
        kputs("no LAN91C111 (revision read back 0x");
        kputhex16((u16)v);
        kputs(")\n");
    }

    /* --- video ---------------------------------------------------- */
    status("video");
    v = SM501_RD(SM501_DEVICEID);
    if (v == SM501_DEVICEID_VALUE) {
        kputs("SM501, device id 0x");
        kputhex32(v);
        kputs(", 16 MiB at 0xf0000000\n");
    } else {
        kputs("no SM501 (device id read back 0x");
        kputhex32(v);
        kputs(")\n");
    }
}
