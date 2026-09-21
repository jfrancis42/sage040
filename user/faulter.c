/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * faulter - try to touch what a program is not allowed to touch.
 *
 * Every other program here is written to work. This one is written to
 * fail, because a claim like "programs run in their own address space"
 * is only worth what can be demonstrated, and the demonstration is that
 * the things a program must not be able to do actually kill it.
 *
 * Each mode makes one forbidden access. If the memory system is doing
 * its job the program never returns from it: the access faults, the
 * kernel kills the program, and the shell prints what happened. If the
 * message after the access DOES appear, protection is not working and
 * the test fails on the presence of that line -- which is the right way
 * round, because a missing fault is silent and a printed line is not.
 *
 *   faulter kernel   read the kernel's own memory
 *   faulter vectors  read address 0, where the vector table lives
 *   faulter device   read the UART's registers
 *   faulter video    read the framebuffer
 *   faulter gap      write to the unmapped hole below its own stack
 *   faulter wild     read an address in no map at all
 *   faulter ok       read and write its OWN memory -- this one must work
 */
#include "ulib.h"

static void try_read(const char *what, u32 addr)
{
    volatile u32 *p = (volatile u32 *)addr;
    u32 v;

    puts("reading ");
    puts(what);
    puts(" at 0x");
    puthex(addr);
    puts("\n");

    v = *p;

    /* Only reached if the access was allowed. */
    puts("NOT-PROTECTED: read 0x");
    puthex(v);
    puts("\n");
}

static void try_write(const char *what, u32 addr)
{
    volatile u32 *p = (volatile u32 *)addr;

    puts("writing ");
    puts(what);
    puts(" at 0x");
    puthex(addr);
    puts("\n");

    *p = 0xdeadbeefUL;

    puts("NOT-PROTECTED: the write was allowed\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "ok";

    if (strcmp(mode, "kernel") == 0) {
        /* The kernel's text. It is definitely there -- the machine is
         * running it -- which is what makes this a real test rather
         * than a read of empty space. */
        try_read("kernel text", 0x00000400UL);

    } else if (strcmp(mode, "vectors") == 0) {
        try_read("the vector table", 0x00000000UL);

    } else if (strcmp(mode, "device") == 0) {
        /* The transparent translation register covering the I/O block
         * is marked supervisor only, so this does not match it and
         * falls through to a page table with nothing in it. */
        try_read("the UART", 0xff000000UL);

    } else if (strcmp(mode, "video") == 0) {
        try_read("video memory", 0xf0000000UL);

    } else if (strcmp(mode, "gap") == 0) {
        /* Between the image and the stack. Not merely unused: unmapped,
         * so a runaway stack lands here and stops. */
        try_write("the gap below the stack", 0x10100000UL);

    } else if (strcmp(mode, "wild") == 0) {
        try_read("nowhere in particular", 0x40000000UL);

    } else if (strcmp(mode, "badptr") == 0) {
        /*
         * The other half of the bargain, and the more important half.
         *
         * A program that faults on its own is its own problem. A program
         * that hands the KERNEL a bad pointer is the kernel's problem,
         * and the only acceptable outcome is an error code: if a system
         * call could be made to fault by passing it rubbish, any program
         * could stop the machine at will.
         *
         * So this makes four calls with pointers that are not mapped,
         * and expects four errors and a program that is still running
         * afterwards.
         */
        s32 r;
        int bad = 0;

        r = write(1, (const void *)0x00000400UL, 16);
        puts("write(kernel ptr)  = "); putdec((u32)-r); puts("\n");
        if (r >= 0) {
            bad = 1;
        }

        r = (s32)open((const char *)0x40000000UL, 0);
        puts("open(wild ptr)     = "); putdec((u32)-r); puts("\n");
        if (r >= 0) {
            bad = 1;
        }

        r = (s32)uname((struct utsname *)0x00000000UL);
        puts("uname(null)        = "); putdec((u32)-r); puts("\n");
        if (r >= 0) {
            bad = 1;
        }

        r = read(0, (void *)0x10100000UL, 16);
        puts("read(into the gap) = "); putdec((u32)-r); puts("\n");
        if (r >= 0) {
            bad = 1;
        }

        if (bad) {
            puts("NOT-PROTECTED: a bad pointer was accepted\n");
        } else {
            puts("BADPTR-ALL-REFUSED\n");
        }
        return 0;

    } else {
        /* The control: its own memory, which must work. If this one
         * faults the test is measuring a broken loader, not a working
         * MMU. */
        static u32 own;
        u32 v;

        own = 0x5a5a5a5aUL;
        v = own;
        puts("own memory reads 0x");
        puthex(v);
        puts("\n");
        if (v == 0x5a5a5a5aUL) {
            puts("OWN-MEMORY-OK\n");
        }
        return 0;
    }

    return 0;
}
