/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * main.c - kernel startup.
 *
 * The order is forced by dependency, and each step earns its place:
 *
 *   1. the console driver, so everything after it can say what happened,
 *      and so descriptors 0, 1 and 2 exist before anything writes to one;
 *   2. the vector table, so a fault from here on is reported instead of
 *      silently jumping into whatever was at address 0;
 *   3. the system call gate, checked by making a call through it;
 *   4. the remaining drivers, each registering a device;
 *   5. the filesystem type, and a mount of it on the disk;
 *   6. the shell.
 *
 * Nothing below step 4 mentions a chip. main() names the drivers because
 * something has to -- this is a board with parts soldered to it, not a
 * bus that can be enumerated -- and that is the only place a part is
 * named at all.
 *
 * Interrupts stay masked until the very end, and are then enabled by the
 * last thing kmain() does before the shell. Up to that point a fault is
 * reported by a handler with the console entirely to itself; an interrupt
 * arriving in the middle of bringing a driver up would be a much harder
 * thing to understand. The console and the disk are polled either way.
 */
#include "kernel.h"
#include "console.h"
#include "dev.h"
#include "vfs.h"
#include "syscall.h"
#include "errno.h"
#include "time.h"
#include "timer.h"
#include "fb.h"
#include "drivers/drivers.h"

static void banner(void)
{
    kputs("\n\n");
    kputs(KERNEL_NAME " kernel ");
    kputs(kernel_version);
    kputs("  (built ");
    kputs(kernel_build);
    kputs(")\n");
    kputs("Copyright (C) 2026 Jeff Francis.  GPL-3.0-or-later.\n\n");
}

static void status(const char *label)
{
    int n = 0;

    kputs("  ");
    kputs(label);
    while (label[n]) {
        n++;
    }
    while (n++ < 8) {
        kputc(' ');
    }
    kputs(": ");
}

/*
 * Make a system call the way a program will, and check the answer came
 * back. The gate is the only path into the kernel that unprivileged code
 * will have, so it is worth knowing it works at the point it is
 * installed rather than at the point something first depends on it.
 */
static void check_syscall_gate(void)
{
    struct utsname u;
    s32 r;

    status("syscall");
    kputs("TRAP #0, Linux/m68k convention, ");

    if (sys_uname(&u) != 0) {
        kputs("uname() FAILED\n");
        return;
    }
    r = syscall1(9999, 0);
    if (r != -ENOSYS) {
        kputs("an unknown call was not refused\n");
        return;
    }
    kputs("verified\n");
}

static void start_drivers(void)
{
    int err;

    status("disk");
    err = ata_init();
    if (err < 0) {
        kputs("none: ");
        kputs(strerror(err));
        kputc('\n');
    } else {
        struct blockdev *b = dev_first_block();

        kputs(b->name);
        kputs(" '");
        kputs(b->model);
        kputs("', ");
        kputdec(b->sectors);
        kputs(" sectors (");
        kputdec(b->sectors / 2048);
        kputs(" MiB)\n");
    }

    status("clock");
    err = m48t59_init();
    if (err < 0) {
        kputs("none: ");
        kputs(strerror(err));
        kputc('\n');
    } else {
        struct tm now;
        time_t secs = sys_time(0);

        gmtime_r(secs, &now);
        kputs(dev_rtc()->name);
        kputs(", ");
        kputdec(now.tm_year + 1900);
        kputc('-');
        kput2((u32)now.tm_mon + 1);
        kputc('-');
        kput2((u32)now.tm_mday);
        kputc(' ');
        kput2((u32)now.tm_hour);
        kputc(':');
        kput2((u32)now.tm_min);
        kputc(':');
        kput2((u32)now.tm_sec);
        kputs(" UTC\n");
    }

    status("timer");
    err = mfp_init();
    if (err < 0) {
        kputs("no MC68901: ");
        kputs(strerror(err));
        kputs(" -- nothing will be able to sleep\n");
    } else if (timer_start() < 0) {
        kputs("MC68901 found but the tick would not start\n");
    } else {
        kputs(dev_timer()->name);
        kputs(" at ");
        kputdec(dev_timer()->hz);
        kputs(" Hz, HZ=");
        kputdec(HZ);
        kputc('\n');
    }

    status("video");
    err = sm501_init();
    if (err < 0) {
        kputs("no SM501: ");
        kputs(strerror(err));
        kputc('\n');
    } else {
        struct fbdev *f = dev_first_fb();

        err = fb_init();
        if (err < 0) {
            kputs("found, but /dev/fb0 would not register: ");
            kputs(strerror(err));
            kputc('\n');
        } else {
            kputs("SM501 as /dev/");
            kputs(f->name);
            kputs(", ");
            kputdec(f->width);
            kputc('x');
            kputdec(f->height);
            kputc('x');
            kputdec(f->bpp);
            kputs(", double buffered\n");
        }
    }

    status("network");
    smc91c111_init();
    if (dev_first_net()) {
        struct netdev *n = dev_first_net();
        int i;

        kputs(n->name);
        kputs(", ");
        for (i = 0; i < NET_ADDR_LEN; i++) {
            kputhex8(n->mac[i]);
            if (i < NET_ADDR_LEN - 1) {
                kputc(':');
            }
        }
        kputc('\n');
    } else {
        kputs("none\n");
    }
}

static void mount_root(void)
{
    struct statfs sf;
    int err;

    status("root");

    err = fat16_init();
    if (err < 0 && err != -EEXIST) {
        kputs("could not register the filesystem type: ");
        kputs(strerror(err));
        kputc('\n');
        return;
    }

    if (!dev_first_block()) {
        kputs("no block device to mount\n");
        return;
    }

    err = vfs_mount("fat16", dev_first_block()->name);
    if (err < 0) {
        kputs("mount failed: ");
        kputs(strerror(err));
        kputs("\n            the disk is readable but carries no FAT16 "
              "volume this kernel can use\n");
        return;
    }

    kputs(vfs_fs_name());
    kputs(" on /dev/");
    kputs(vfs_dev_name());
    if (sys_statfs(&sf) == 0) {
        kputs(" '");
        kputs(sf.f_label[0] ? sf.f_label : "(unlabelled)");
        kputs("', ");
        kputdec((sf.f_blocks * sf.f_bsize) / 1024);
        kputs(" KB, ");
        kputdec((sf.f_bfree * sf.f_bsize) / 1024);
        kputs(" KB free, ");
        kputdec(sf.f_bsize);
        kputs(" byte clusters");
    }
    kputc('\n');
}

void kmain(void)
{
    /* The console first, and through the driver model like everything
     * else -- it registers /dev/console and binds descriptors 0, 1, 2. */
    if (ns16550_init() < 0) {
        halt();                 /* nothing could report this anyway */
    }
    banner();

    trap_init();
    status("traps");
    kputs("256 vectors at 0x");
    kputhex32((u32)_vectors);
    kputs(", TRAP #0 is the system call gate\n");

    check_syscall_gate();
    probe_all();
    start_drivers();
    mount_root();

    /*
     * Interrupts last. Up to this point a fault is reported by a handler
     * with the console to itself; an interrupt arriving in the middle of
     * bringing a driver up would be a much harder thing to understand.
     */
    if (dev_timer()) {
        mfp_interrupts_on();
    }

    kputs("\nkernel ready.  'help' lists commands.\n\n");
    shell();
}
