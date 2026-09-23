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
#include "pmm.h"
#include "vm.h"
#include "net.h"
#include "random.h"
#include "dev.h"
#include "vfs.h"
#include "syscall.h"
#include "errno.h"
#include "time.h"
#include "timer.h"
#include "fb.h"
#include "memdev.h"
#include "klog.h"
#include "pty.h"
#include "fbcon.h"
#include "tty.h"
#include "task.h"
#include "drivers/drivers.h"

static void banner(void)
{
    kputs("\n\n");
    kputs(KERNEL_NAME " ");
    kputs(kernel_version);
    kputs(" on " MACHINE_NAME "  (built ");
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

/*
 * Physical memory, then the MMU.
 *
 * Before the drivers, because after this the kernel is running
 * translated and anything that comes up afterwards comes up in the world
 * it will live in -- rather than being brought up in one addressing
 * model and then having the ground moved underneath it.
 *
 * The pool starts at the end of the kernel IMAGE -- which now includes
 * the 64 KB boot supervisor stack, reserved by kernel.ld -- and runs to
 * the end of RAM. Nothing hands out a page the kernel is standing on,
 * which is the only reason it is safe to give a page to a program that
 * will write anything it likes to it.
 */

static void start_memory(void)
{
    u32 ram = probe_memory();
    u32 first = PAGE_ALIGN_UP((u32)_end);
    /*
     * Everything from the end of the kernel image to the end of RAM.
     *
     * There is no region set aside for programs: a program's image, its
     * stack and its page tables all come from here like everything
     * else, and where they physically land is the allocator's business
     * rather than a constant in a header.
     *
     * `_end` is past the boot supervisor stack, which is reserved
     * inside the image by kernel.ld -- so the allocator cannot hand out
     * the stack it is running on. This used to run to just under a
     * stack pinned at 4 MB, which capped the usable machine at 4 MB no
     * matter how much RAM there was.
     */
    u32 last  = PAGE_ALIGN_DOWN(ram);

    pmm_init(first, last);

    status("pages");
    kputdec(pmm_total());
    kputs(" of ");
    kputdec((u32)PAGE_SIZE / 1024);
    kputs(" KB free from 0x");
    kputhex32(first);
    kputs(" to 0x");
    kputhex32(last);
    kputc('\n');

    vm_init(ram);

    status("mmu");
    kputs("on, 4 KB pages, kernel identity-mapped supervisor-only, ");
    kputdec(pmm_total() - pmm_available());
    kputs(" pages of tables\n");
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

    /*
     * The serial port has been the console since the start, polled. Now
     * the MFP is up it can have its receive interrupt -- not before, as
     * mfp_init() clears every handler and enable it finds.
     */
    if (dev_timer()) {
        ns16550_irq_on();
        ata_irq_on();
    }

    /* Not hardware, so nothing to probe for: /dev/null and friends. */
    if (memdev_init() < 0) {
        kputs("/dev/null and the other memory devices would not register\n");
    }

    /*
     * /dev/klog, so that what the kernel has said can be read back.
     * The RING has been filling since the first message -- klog.c needs
     * no initialising for that -- and this only makes it reachable by
     * name, which is why it can happen here rather than first.
     */
    klog_init();

    /*
     * /dev/ptmx: pseudo-terminals, which is how a program gives another
     * program a terminal of its own. Nothing here has one until
     * something asks.
     */
    pty_init();

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

            err = fbcon_init();
            if (err < 0) {
                kputs("            no text console: ");
                kputs(strerror(err));
                kputc('\n');
            } else {
                status("fbcon");
                kputs("/dev/fbcon, ");
                kputdec((u32)fbcon_cols());
                kputc('x');
                kputdec((u32)fbcon_rows());
                kputs(" of IBM PC 8x16, green on black\n");
            }
        }
    }

    status("keyboard");
    err = i8042_init();
    if (err < 0) {
        kputs("no 8042: ");
        kputs(strerror(err));
        kputs(" -- input from the serial line only\n");
    } else {
        kputs("8042 as /dev/kbd0, scancode set 1, US layout\n");
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

/*
 * What the terminal ended up with.
 *
 * Printed after every driver has had its turn rather than as each one
 * registers, because a list is only worth printing once it is complete
 * -- reporting one source and then acquiring another is how a boot log
 * ends up disagreeing with the machine.
 */
/*
 * Bring the interface up.
 *
 * After the drivers, because it needs one; before the root filesystem
 * only so that the report reads in a sensible order. No address is
 * configured -- that is `ifconfig`, or DHCP when there is some -- so
 * what this does is enable the receiver and start answering ARP for
 * nothing at all.
 */
static void start_network(void)
{
    struct netif *n;
    int err;

    /*
     * Seeded here because this is the first moment the things it seeds
     * from exist: the clock has been read, the ethernet address is
     * known, and the tick has been running long enough to have counted
     * something that depended on how the disk behaved.
     */
    random_init();

    err = net_init();

    status("net");
    if (err < 0) {
        kputs("none: ");
        kputs(strerror(err));
        kputc('\n');
        return;
    }
    n = net_if();
    kputs(n->dev->name);
    kputs(" up, ethernet + ARP, no address yet (try `ifconfig`)\n");
}

static void report_console(void)
{
    struct chardev *d;
    int i, on;

    status("console");
    kputs("output to");
    for (i = 0; (d = tty_sink(i, &on)) != 0; i++) {
        kputc(' ');
        kputs(d->name);
        if (!on) {
            kputs("(off)");
        }
    }
    kputs(", input from");
    for (i = 0; (d = tty_source(i)) != 0; i++) {
        kputc(' ');
        kputs(d->name);
    }
    kputc('\n');
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

    /*
     * A volume that was not put away properly -- the machine was reset,
     * or the emulator killed -- is checked, and repaired, before anything
     * uses it: what a Linux distribution does with fsck -p at boot.
     */
    {
        struct fsck_report r;

        err = vfs_check(FSCK_REPAIR | FSCK_IF_DIRTY, &r);
        if (err < 0) {
            status("fsck");
            kputs("could not check the volume: ");
            kputs(strerror(err));
            kputc('\n');
        } else if (r.was_dirty) {
            u32 found = r.fat_mismatch + r.bad_chains + r.cross_linked +
                        r.size_fixed + r.dot_entries + r.orphan_lfn +
                        r.lost_clusters;

            status("fsck");
            kputs("not cleanly unmounted; checked ");
            kputdec(r.files);
            kputs(" files in ");
            kputdec(r.dirs + 1);
            kputs(" directories: ");
            if (found == 0) {
                kputs("clean\n");
            } else {
                kputdec(found);
                kputs(" problems, ");
                kputdec(r.fixed);
                kputs(" repairs\n");
            }
        }
    }
}

void kmain(void)
{
    /*
     * The serial port first, then the terminal on top of it. Nothing can
     * report a failure before both are up, so neither gets to fail
     * politely: the port is the console of last resort and the terminal
     * is what everything writes through.
     */
    /*
     * Tasks before anything else, and before the console in particular.
     *
     * Not because the scheduler is needed this early -- nothing is
     * scheduled for a long time yet -- but because DESCRIPTORS LIVE IN A
     * TASK. tty_init() binds 0, 1 and 2, and with no current task that
     * wrote through a null pointer into the vector table, which is
     * mapped and writable and therefore did not fault. The machine came
     * up, printed its banner, and died at the first exception.
     *
     * This costs nothing: task_init() only claims the context the kernel
     * is already running in, and allocates nothing.
     */
    task_init();

    if (ns16550_init() < 0) {
        halt();                 /* nothing could report this anyway */
    }
    if (tty_init() < 0) {
        halt();
    }
    banner();

    trap_init();
    status("traps");
    kputs("256 vectors at 0x");
    kputhex32((u32)_vectors);
    kputs(", TRAP #0 is the system call gate\n");

    check_syscall_gate();
    probe_all();
    start_memory();
    start_drivers();
    report_console();
    start_network();
    mount_root();

    /*
     * Interrupts last. Up to this point a fault is reported by a handler
     * with the console to itself; an interrupt arriving in the middle of
     * bringing a driver up would be a much harder thing to understand.
     */
    if (dev_timer()) {
        mfp_interrupts_on();
    }

    /*
     * The shell becomes a task, and the startup code becomes the idle
     * loop by falling into schedule().
     *
     * Note the order: task_init() first, because every task needs a
     * kernel stack and the allocator has to be up, and the shell last,
     * because it is the thing that expects everything else to work.
     */


    /*
     * The network's own task. Protocol work -- frames that arrived, and
     * TCP's retransmission, delayed-ACK and TIME_WAIT timers -- used to
     * happen only while some program sat in a socket call. A connection
     * nobody happened to be reading then ACKed nothing and made its
     * peer retransmit. netd does that work fifty times a second whether
     * or not anybody is asking, and because it is a KERNEL task, which
     * is never preempted, the stack still needs no locking.
     */
    if (!task_create("netd", net_task)) {
        kputln("could not start netd");
    }

    {
        struct task *sh = task_create("sh", shell);

        if (sh) {
            tty_set_foreground(sh->pid);
        }
        if (!sh) {
            kputln("could not start the shell");
            halt();
        }
    }

    kputs("\nkernel ready.  'help' lists commands.\n\n");

    /*
     * And this becomes the idle loop.
     *
     * schedule() first, because something may already be ready; then
     * STOP, which puts the processor to sleep until an interrupt, so an
     * idle machine costs the host nothing and would cost real hardware
     * no power. The interrupt that wakes it is also what marks a task
     * ready, so the loop finds work waiting for it.
     */
    for (;;) {
        schedule();
        __asm__ volatile ("stop #0x2000");
    }
}
