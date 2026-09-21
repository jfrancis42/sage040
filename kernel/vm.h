/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * vm.h - address spaces.
 *
 * Reference: Motorola M68040 User's Manual, chapter 3.
 *
 * THE SHAPE OF IT
 *
 * The 68040 has two root pointers, and which one is used depends on the
 * function code of the access, not on anything software chooses at the
 * time. Supervisor accesses walk SRP; user accesses walk URP. That one
 * fact decides the whole design:
 *
 *   SRP -> the kernel's map. Identity, all of RAM, supervisor only.
 *          Built once at startup and never changed again.
 *
 *   URP -> the running program's map. Its own pages, at its own virtual
 *          addresses, user accessible. Swapped on every spawn and on
 *          every resume, and that swap is the whole of "its own address
 *          space".
 *
 * Because the kernel's map is identity and covers all of RAM, the kernel
 * can reach any physical page by its physical address -- including every
 * page belonging to every program. That is what lets it load an image,
 * build a program's tables, and copy a system call's arguments, all
 * without mapping anything specially. It is also why a page handed to a
 * program is not protected FROM the kernel, only from other programs;
 * the kernel is trusted, and on a machine with no supervisor/user split
 * in the hardware below this, it has to be.
 *
 * A program's virtual addresses are NOT reachable by simply
 * dereferencing them in the kernel: 0x10001234 means one thing to the
 * program and nothing at all to the supervisor map, where it is
 * unmapped. That is deliberate. A kernel bug that uses a user pointer
 * raw faults immediately instead of quietly reading kernel memory that
 * happens to live at the same number -- which is exactly what would have
 * happened had programs stayed at 1 MB. Reaching a program's memory goes
 * through uaccess.h, which walks its tables and says no if the page is
 * not there.
 *
 * THE MEMORY MAP
 *
 *   supervisor (SRP)                 user (URP)
 *   0x00000000  vectors              0x10000000  program image
 *   0x00000400  kernel               ...
 *   ...         all of RAM, identity  0x101f0000  program stack
 *   0x003ffff0  supervisor stack      0x10200000  end
 *
 *   0xf0000000  SM501 VRAM  ] through the transparent translation
 *   0xff000000  I/O         ] registers, supervisor only, uncached
 *
 * Everything outside those ranges is unmapped in both, so a wild pointer
 * lands on nothing rather than on something.
 *
 * WHY THE TRANSPARENT TRANSLATION REGISTERS
 *
 * The 68040 can map a power-of-two range without a table entry, which is
 * what ITT0/ITT1 and DTT0/DTT1 are. Using them for the device block and
 * the framebuffer keeps 32 MB of I/O out of the page tables entirely,
 * and each carries its own supervisor-only bit -- so a user program that
 * reaches for the UART does not match the register, falls through to its
 * own tables, and faults. It does not get the device.
 *
 * ITT0 does the same for supervisor instruction fetch across RAM. That
 * one is a safety net rather than a necessity: turning the MMU on is a
 * cliff, because the instruction after the one that enables it has to be
 * mapped or the machine is gone with no way to find out why. With ITT0
 * covering kernel text, enabling the MMU cannot break instruction fetch,
 * and a mistake in the tables shows up as a data fault the kernel is
 * still alive to report.
 */
#ifndef VM_H
#define VM_H

#include "kernel.h"
#include "pmm.h"

/*
 * Where a program lives, in its own address space.
 *
 * Well clear of the kernel on purpose. The old layout put programs at
 * 1 MB, inside the range the kernel identity-maps, so a kernel bug that
 * dereferenced a user pointer read kernel memory and carried on. At
 * 0x10000000 there is nothing behind it in the supervisor map and the
 * same bug is a bus error with an address that says what happened.
 *
 * Two megabytes, which is 8 pointer-table entries and 8 page tables --
 * and that number is load-bearing: an address space's root, pointer and
 * page tables are packed into ONE physical page, and eight is what fits.
 * Growing the user area past 2 MB means that packing changes.
 */
#define USER_VA_BASE    0x10000000UL
#define USER_VA_SIZE    0x00200000UL    /* 2 MB */
#define USER_VA_END     (USER_VA_BASE + USER_VA_SIZE)

#define USER_PTABLES    8               /* 8 x 256 KB = USER_VA_SIZE */

/*
 * The stack lives at the top, with a deliberate hole below it.
 *
 * Named apart from exec.h's old USER_STACK_TOP on purpose: one is a
 * physical address in a shared space and the other is a virtual address
 * in a private one, and having both spelled the same during the change
 * from one to the other is how the wrong one gets used.
 */
#define USER_STACK_PAGES  16            /* 64 KB */
#define USER_VA_STACK_TOP (USER_VA_END - 16)

/* What a mapping is allowed to do. */
#define VM_USER     0x01        /* reachable from user mode        */
#define VM_WRITE    0x02        /* writable                        */
#define VM_NOCACHE  0x04        /* device memory, not cached       */

struct addrspace {
    u32 root;                   /* physical address of the root table */
    u32 page;                   /* the one page holding all its tables */
    int used;
};

/*
 * Build the kernel's map and turn the MMU on. Does not return if it
 * cannot: a kernel that thinks it enabled protection and did not is
 * worse than one that stops and says so.
 */
void vm_init(u32 ram_bytes);

int  vm_enabled(void);

/* A fresh, empty user address space, or null if there is no memory. */
struct addrspace *vm_create(void);

/* Give back every page it owns, including the pages mapped into it. */
void vm_destroy(struct addrspace *as);

/*
 * Map one page. `pa` of zero means "allocate one", which is what every
 * caller wants -- the point of a private address space is that the
 * program does not choose its own physical memory.
 *
 * Returns the physical address mapped, or 0.
 */
u32  vm_map(struct addrspace *as, u32 va, u32 pa, int flags);

/*
 * What physical address does this user address mean?
 *
 * Returns 0 if it is not mapped, or if `write` is set and the page is
 * read only. This is the software version of the walk the hardware does
 * and it exists for uaccess.c: the kernel cannot simply dereference a
 * user pointer, because user addresses mean nothing in the supervisor
 * map.
 */
u32  vm_translate(struct addrspace *as, u32 va, int write);

/* Make this the address space user mode sees. Null means none, which is
 * what the kernel runs with when no program is loaded. */
void vm_switch(struct addrspace *as);

u32  vm_mapped_pages(struct addrspace *as);

/*
 * Take a page out of the kernel's own map, or put it back.
 *
 * Used for one thing: the guard page under a program's supervisor
 * stack. The kernel maps all of RAM, so a kernel stack that overflows
 * would otherwise run quietly into whatever is below it -- and a kernel
 * stack overflow that corrupts something else and carries on is about
 * the worst failure a system can have. Removing the page below it turns
 * that into an access fault at the moment it happens.
 */
void vm_kernel_present(u32 va, int present);

#endif /* VM_H */
