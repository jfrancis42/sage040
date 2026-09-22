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
 *   0x00000400  kernel, its stack    ...         unmapped gap
 *   ...         all of RAM, identity  0x1ff00000  program stack, 1 MB
 *                                     0x20000000  end
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
 * TWO HUNDRED AND FIFTY-SIX MEGABYTES, and the number is not the
 * interesting part -- how the tables are managed is.
 *
 * It was 2 MB, because an address space's root, pointer and page tables
 * were packed into ONE physical page and eight page tables is what fits.
 * That made creating an address space one allocation and destroying it
 * one free, which was a good trade when a program was a small thing at
 * a fixed address. It stopped being one the moment a program needed to
 * ask for memory: 2 MB is smaller than a single useful program, and
 * `brk` inside it is a rounding error.
 *
 * Mapping 256 MB eagerly would cost 1024 page tables -- and the tables
 * for an address space nobody has touched are pure waste. So tables are
 * allocated ON DEMAND, in 512-byte slots carved from pages that are
 * threaded onto a list so they can be freed. An address space costs
 * what it uses: a program with 100 KB of image and its 1 MB stack
 * needs eight tables -- a root, two pointer tables (the image and the
 * stack are under different 32 MB root entries), one page table for
 * the image and four for the stack -- which is two pages.
 */
#define USER_VA_BASE    0x10000000UL
#define USER_VA_SIZE    0x10000000UL    /* 256 MB */
#define USER_VA_END     (USER_VA_BASE + USER_VA_SIZE)

/*
 * The stack lives at the top, with a deliberate hole below it.
 *
 * Named apart from exec.h's old USER_STACK_TOP on purpose: one is a
 * physical address in a shared space and the other is a virtual address
 * in a private one, and having both spelled the same during the change
 * from one to the other is how the wrong one gets used.
 */
#define USER_STACK_PAGES  256           /* 1 MB */
#define USER_VA_STACK_TOP (USER_VA_END - 16)

/* What a mapping is allowed to do. */
#define VM_USER     0x01        /* reachable from user mode        */
#define VM_WRITE    0x02        /* writable                        */
#define VM_NOCACHE  0x04        /* device memory, not cached       */
#define VM_NONE     0x08        /* owned but inaccessible: vm_protect */

struct addrspace {
    u32 root;                   /* physical address of the root table  */

    /*
     * Every page this address space has taken for tables, threaded
     * through the first word of each. The rest of the page is carved
     * into 512-byte slots. Destroying an address space walks this list
     * rather than trying to work out which pages were tables, which is
     * not recoverable from the tables themselves.
     */
    u32 tables;                 /* head of that list, 0 when empty     */
    u32 slot_page;              /* the page being carved right now     */
    u32 slot_off;               /* next free offset within it          */

    /*
     * The program break: where the heap starts and where it currently
     * ends. A property of the address space, not of the task, because
     * it describes what is mapped. brk_start is set by exec to the page
     * after the image and never moves; brk_cur is what brk() returns.
     */
    u32 brk_start;
    u32 brk_cur;

    int used;

    /*
     * How many TASKS are using it. One for an ordinary process; one per
     * thread for a threaded one, because CLONE_VM shares the space
     * rather than copying it. vm_destroy() at anything above one is a
     * thread letting go, not the space being torn down -- which is what
     * makes the LAST thread out the one that frees the memory.
     */
    int refs;

    /*
     * Still being built -- by exec, before it is anybody's. Reclaim
     * leaves such a space alone: its pages are being filled through
     * their physical addresses, and one evicted from under the loader
     * would take the loaded bytes with it. vm_ready() clears it.
     */
    int busy;
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

/*
 * A new address space holding a private copy of every page of `src`,
 * with the same protections, for fork(). Null if there is not the
 * memory; nothing is left allocated in that case.
 */
struct addrspace *vm_clone(struct addrspace *src);

/* One more task is using this space. Returns it, for convenience. */
struct addrspace *vm_share(struct addrspace *as);

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

/* Remove one page, giving its memory back. Nothing if it was not mapped. */
void vm_unmap(struct addrspace *as, u32 va);

/* Does the address space own the page at `va`, accessible or not? */
int  vm_is_mapped(struct addrspace *as, u32 va);

/*
 * Change what may be done to an owned page: VM_WRITE for read-write,
 * 0 for read-only, VM_NONE for nothing at all. There is no execute
 * permission to change, because the 68040 has no bit for it. Returns 0,
 * or -1 if the page is not owned.
 */
int  vm_protect(struct addrspace *as, u32 va, int flags);

/*
 * DEMAND PAGING (task 21). A page need not exist to be mapped: it can
 * be LAZY -- zeroes, made on first touch -- or out in the swap file.
 * Both are descriptors the MMU sees as invalid, so touching one faults,
 * and vm_fault() makes the page and returns, and the 68040 runs the
 * faulting instruction again.
 */

/* Map `va` lazily: no page until it is touched. 0, or -1 for no tables. */
int  vm_map_lazy(struct addrspace *as, u32 va, int flags);

/*
 * Resolve a fault on `va`: make a lazy page, bring a swapped one back,
 * or copy a copy-on-write one for writing. 0 when it changed something
 * and the access can now succeed; VM_FAULT_NOCHANGE when the page was
 * already there and accessible (a stale translation, flushed); -EFAULT
 * when it never could (unmapped, read-only, PROT_NONE); -ENOMEM when
 * there is no page to be had.
 */
#define VM_FAULT_NOCHANGE   1
int  vm_fault(struct addrspace *as, u32 va, int write);

/* May the program write here -- now, or after a fault (copy-on-write,
 * lazy, swapped)? What the program sees, not what the MMU does. */
int  vm_may_write(struct addrspace *as, u32 va);

/* The address space is complete and belongs to a task: fair game for
 * reclaim from now on. */
void vm_ready(struct addrspace *as);

/*
 * Free at least `want` pages if it can: first the file-page cache's idle
 * pages, then -- with swap on -- the least recently used pages of every
 * address space, written to the swap file. Returns how many it freed.
 */
u32  vm_reclaim(u32 want);

/* Bring every swapped page back in, for swapoff. 0, or -ENOMEM. */
int  vm_swapoff(void);

/* Could `pages` more be promised to programs? Free memory and free swap
 * together, less the kernel's reserve. */
int  vm_commit_ok(u32 pages);

struct vm_stats {
    u32 faults_zero;            /* lazy pages made                     */
    u32 faults_cow;             /* copy-on-write resolved              */
    u32 faults_swapin;          /* pages brought back from swap        */
    u32 evicted;                /* pages written out to make room      */
};
void vm_stats(struct vm_stats *out);

/*
 * The highest address the heap may reach: one guard page below the
 * stack, so a heap that meets the stack faults instead of merging.
 */
#define USER_BRK_LIMIT  (USER_VA_END - (u32)(USER_STACK_PAGES + 1) * PAGE_SIZE)

/*
 * Move the program break, with Linux's semantics: returns the NEW break
 * on success and the OLD one, unchanged, on any failure. There is no
 * error return; a caller finds out by comparing. See vm.c.
 */
u32  vm_brk(struct addrspace *as, u32 addr);

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
