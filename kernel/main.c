/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * main.c - kernel startup.
 *
 * Order matters here and each step depends on the one before it:
 *
 *   1. the console, so everything after it can say what happened;
 *   2. the vector table, so a fault from this point on is reported
 *      rather than silently jumping into whatever was at address 0;
 *   3. the TRAP #0 gate, checked by making a call through it;
 *   4. the hardware inventory;
 *   5. the filesystem;
 *   6. the shell.
 *
 * Interrupts stay masked throughout.  Nothing yet needs them -- the
 * console and the disk are both polled -- and turning them on before
 * there is a handler worth running only creates ways to hang.
 */
#include "kernel.h"
#include "fs.h"
#include "block.h"
#include "string.h"

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

/*
 * Make one system call the way a user program will, and check the answer
 * came back.  The gate is the only path into the kernel that unprivileged
 * code will have, so it is worth knowing it works at the point it is
 * installed rather than at the point something first depends on it.
 */
static void check_syscall_gate(void)
{
    static const char marker[] = "";
    s32 r;

    kputs("  syscall : TRAP #0 gate, ");
    r = syscall(SYS_PUTS, (u32)marker, 0);
    if (r != 0) {
        kputs("CALL FAILED\n");
        return;
    }
    r = syscall(999, 0, 0);
    if (r != -1) {
        kputs("bad call number was not rejected\n");
        return;
    }
    kputdec(SYS_NCALLS);
    kputs(" calls, verified\n");
}

static void mount_filesystem(void)
{
    int err;

    kputs("  fs      : ");
    err = fs_mount();
    if (err != FS_OK) {
        kputs("not mounted: ");
        kputs(fs_strerror(err));
        kputs("\n            the disk is readable but carries no FAT16 "
              "volume this kernel can use\n");
        return;
    }

    kputs("FAT16 '");
    kputs(fs_label()[0] ? fs_label() : "(unlabelled)");
    kputs("', ");
    kputdec(fs_total_bytes() / 1024);
    kputs(" KB, ");
    kputdec(fs_free_bytes() / 1024);
    kputs(" KB free, ");
    kputdec(fs_cluster_bytes());
    kputs(" byte clusters\n");
}

void kmain(void)
{
    con_init();
    banner();

    trap_init();
    kputs("  traps   : 256 vectors at 0x");
    kputhex32((u32)_vectors);
    kputs(", TRAP #0 is the system call gate\n");

    check_syscall_gate();
    probe_all();
    mount_filesystem();

    kputs("\nkernel ready.  'help' lists commands.\n\n");
    shell();
}
