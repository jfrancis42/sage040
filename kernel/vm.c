/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * vm.c - page tables, and turning the MMU on.
 *
 * Reference: Motorola M68040 User's Manual, chapter 3.
 *
 * The 4 KB address decomposition, which everything here is built from:
 *
 *   root index    = (va >> 25) & 0x7f     128 entries, 512-byte table
 *   pointer index = (va >> 18) & 0x7f     128 entries, 512-byte table
 *   page index    = (va >> 12) & 0x3f      64 entries, 256-byte table
 *   offset        =  va & 0xfff
 *
 * So one root entry covers 32 MB, one pointer entry 256 KB, and one page
 * table 256 KB.
 *
 * Descriptor bits worth stating rather than looking up:
 *
 *   bits 1-0   UDT/PDT   00 invalid, 01 or 11 resident, 10 indirect
 *   bit 2      W         write protected
 *   bit 3      U         used      -- set by hardware, not by us
 *   bit 4      M         modified  -- set by hardware, not by us
 *   bits 6-5   CM        cache mode
 *   bit 7      S         supervisor only
 *
 * TWO THINGS THE HARDWARE DOES THAT ARE EASY TO FORGET.
 *
 * The CPU WRITES to the page tables: it sets U on every descriptor it
 * walks and M on a page it stores to. Tables therefore have to live in
 * ordinary writable RAM, and a table that looks modified when nothing
 * modified it is the hardware, not a bug.
 *
 * And there is no S bit on a table descriptor. A supervisor-only region
 * has to carry the bit on every one of its PAGE descriptors; there is no
 * way to mark a whole subtree. Getting that wrong does not fail loudly,
 * it just leaves the kernel readable from user mode.
 *
 * WHAT THE EMULATOR DOES NOT DO, which matters for anyone reading this
 * on real hardware: QEMU ignores the CM bits entirely and implements
 * cinv and cpush as no-ops, because it has no cache model. The cache
 * modes below are set correctly anyway -- a real 68040 with its device
 * registers marked cachable would be a machine that reads a stale UART
 * status forever, and the day this runs on one is not the day to find
 * that out.
 */
#include "vm.h"
#include "pmm.h"
#include "console.h"
#include "string.h"

/* --- descriptor bits ----------------------------------------------- */

#define UDT_RESIDENT    0x002
#define PDT_RESIDENT    0x001

#define DESC_WP         0x004           /* write protected              */
#define DESC_CM_CB      0x020           /* cachable, copyback           */
#define DESC_CM_NC      0x060           /* noncachable                  */
#define DESC_SUPER      0x080           /* supervisor only              */

/*
 * A page a program owns but may not touch: mprotect(PROT_NONE).
 *
 * The hardware has no such state -- a page descriptor is resident or it
 * is not -- so it is an INVALID descriptor (type 00) that still carries
 * the physical address, marked with this bit. An invalid descriptor is
 * ignored by the MMU apart from its type field, so every other bit is
 * software's to use, and an access to the page faults exactly as an
 * unmapped one would. The memory stays owned: it is counted, freed on
 * exit, and comes back intact when the protection is lifted.
 */
#define DESC_SW_NONE    0x800

#define PTR_TABLE_MASK  0xfffffe00UL    /* root descriptor -> pointer table */
#define PAGE_TABLE_MASK 0xffffff00UL    /* pointer descriptor -> page table */
#define PAGE_ADDR_MASK  0xfffff000UL    /* page descriptor -> the page      */

#define ROOT_INDEX(va)  (((va) >> 25) & 0x7f)
#define PTR_INDEX(va)   (((va) >> 18) & 0x7f)
#define PAGE_INDEX(va)  (((va) >> 12) & 0x3f)

#define ROOT_ENTRIES    128
#define PTR_ENTRIES     128
#define PAGE_ENTRIES    64

#define ROOT_BYTES      (ROOT_ENTRIES * 4)      /* 512 */
#define PTR_BYTES       (PTR_ENTRIES * 4)       /* 512 */
#define PAGE_BYTES      (PAGE_ENTRIES * 4)      /* 256 */

/* --- translation control ------------------------------------------- */

#define TC_ENABLE       0x8000          /* bit 14 clear = 4 KB pages */

/*
 * Transparent translation, which maps a power-of-two range with no table
 * entry at all. Bits 31-24 are the base and bits 23-16 the mask, where a
 * SET mask bit means "do not compare this bit" -- so a mask of 0x00
 * matches one 16 MB block and 0x03 matches 64 MB.
 *
 * Bit 15 enables; bits 14-13 are the mode field, where 01 is supervisor
 * only. Every one of these is supervisor only, which is what keeps a
 * user program away from the device registers: its access does not match
 * the register at all, falls through to its own page tables, and finds
 * nothing there.
 */
#define TT_ENABLE       0x8000
#define TT_SUPER        0x2000

#define ITT0_KERNEL     (0x0003UL << 16 | TT_ENABLE | TT_SUPER | DESC_CM_CB)
#define DTT0_IO         (0xff00UL << 16 | TT_ENABLE | TT_SUPER | DESC_CM_NC)
#define DTT1_VRAM       (0xf000UL << 16 | TT_ENABLE | TT_SUPER | DESC_CM_NC)

static u32 kernel_root;
static int enabled;

/* --- the instructions ---------------------------------------------- */

static void set_tc(u32 v)   { __asm__ volatile("movec %0,%%tc"   :: "d"(v)); }
static void set_srp(u32 v)  { __asm__ volatile("movec %0,%%srp"  :: "d"(v)); }
static void set_urp(u32 v)  { __asm__ volatile("movec %0,%%urp"  :: "d"(v)); }
static void set_itt0(u32 v) { __asm__ volatile("movec %0,%%itt0" :: "d"(v)); }
static void set_itt1(u32 v) { __asm__ volatile("movec %0,%%itt1" :: "d"(v)); }
static void set_dtt0(u32 v) { __asm__ volatile("movec %0,%%dtt0" :: "d"(v)); }
static void set_dtt1(u32 v) { __asm__ volatile("movec %0,%%dtt1" :: "d"(v)); }

static u32 get_tc(void)
{
    u32 v;

    __asm__ volatile("movec %%tc,%0" : "=d"(v));
    return v;
}

/*
 * Throw the translation cache away.
 *
 * REQUIRED AFTER EVERY ONE OF THE ABOVE, and after changing any
 * descriptor. Writing a root pointer does not invalidate the
 * translations made through the old one -- not on a real 68040, and not
 * under QEMU either, whose movec helper does no flushing at all. A
 * missing flush here does not fail immediately; it fails later, on some
 * unrelated access that happened to be cached, which is close to
 * undebuggable. So every path that touches a table or a register ends
 * here.
 */
static void pflusha(void)
{
    __asm__ volatile("pflusha" ::: "memory");
}

/* --- word access to a table ---------------------------------------- */

static u32 *table(u32 pa)
{
    /*
     * Physical addresses are kernel addresses: the kernel's map is
     * identity for all of RAM. This function is the one place that
     * assumption is spelled out, so that the day the kernel stops
     * mapping all of memory there is one thing to change.
     */
    return (u32 *)pa;
}

/* --- building a mapping -------------------------------------------- */

/* Does this descriptor hold a page the address space owns? */
static int desc_owned(u32 d)
{
    return (d & PDT_RESIDENT) || (d & DESC_SW_NONE);
}

static u32 page_bits(int flags)
{
    u32 d = PDT_RESIDENT;

    if (!(flags & VM_USER)) {
        d |= DESC_SUPER;
    }
    if (!(flags & VM_WRITE)) {
        d |= DESC_WP;
    }
    d |= (flags & VM_NOCACHE) ? DESC_CM_NC : DESC_CM_CB;
    return d;
}

/*
 * Tables for the kernel's own map.
 *
 * A bump allocator over whole pages, because the kernel's map is built
 * once and never taken down -- there is nothing to free, so there is no
 * reason to carry the machinery for freeing it. Every chunk is 512-byte
 * aligned whatever its size, which satisfies both the 512-byte alignment
 * a pointer table needs and the 256-byte alignment a page table needs,
 * and costs 256 bytes per page table. That is the cheapest correct thing
 * rather than the smallest.
 */
static u32 ktbl_page;
static u32 ktbl_off;

static u32 ktable_alloc(void)
{
    u32 t;

    if (ktbl_page == 0 || ktbl_off + 512 > PAGE_SIZE) {
        ktbl_page = pmm_alloc();
        if (!ktbl_page) {
            return 0;
        }
        ktbl_off = 0;
    }
    t = ktbl_page + ktbl_off;
    ktbl_off += 512;
    return t;
}

static int kmap_page(u32 va, u32 pa, int flags)
{
    u32 *root = table(kernel_root);
    u32 rd = root[ROOT_INDEX(va)];
    u32 ptr_pa, *ptr, pd, page_pa, *pt;

    if ((rd & UDT_RESIDENT) == 0) {
        ptr_pa = ktable_alloc();
        if (!ptr_pa) {
            return -1;
        }
        root[ROOT_INDEX(va)] = ptr_pa | UDT_RESIDENT;
        rd = root[ROOT_INDEX(va)];
    }
    ptr_pa = rd & PTR_TABLE_MASK;
    ptr = table(ptr_pa);

    pd = ptr[PTR_INDEX(va)];
    if ((pd & UDT_RESIDENT) == 0) {
        page_pa = ktable_alloc();
        if (!page_pa) {
            return -1;
        }
        ptr[PTR_INDEX(va)] = page_pa | UDT_RESIDENT;
        pd = ptr[PTR_INDEX(va)];
    }
    pt = table(pd & PAGE_TABLE_MASK);

    pt[PAGE_INDEX(va)] = (pa & PAGE_ADDR_MASK) | page_bits(flags);
    return 0;
}

/* --- starting up ---------------------------------------------------- */

void vm_init(u32 ram_bytes)
{
    u32 va;

    kernel_root = ktable_alloc();
    if (!kernel_root) {
        kputln("vm: no memory for the root table");
        halt();
    }

    /*
     * All of RAM, identity, supervisor only, writable.
     *
     * Identity because the kernel has to be able to reach any physical
     * page by its address -- every page table it builds and every
     * program image it loads is somewhere in here. Supervisor only
     * because that is the entire point: after this, a user program
     * reaching for kernel memory takes an access fault rather than
     * reading it.
     *
     * Writable, including the text. Write-protecting kernel text would
     * be worth having, but the vector table sits at address 0 in the
     * same page as the start of text and trap_init() writes to it at
     * runtime -- so it would have to be split first, and a half-measure
     * that protected some of the kernel would read as though it
     * protected all of it.
     */
    for (va = 0; va < ram_bytes; va += PAGE_SIZE) {
        if (kmap_page(va, va, VM_WRITE) < 0) {
            kputln("vm: ran out of memory building the kernel map");
            halt();
        }
    }

    /*
     * Devices and video, out of the tables entirely. Supervisor only, so
     * a user access does not match and falls through to a page table
     * that has nothing there.
     */
    set_itt0(ITT0_KERNEL);
    set_itt1(0);
    set_dtt0(DTT0_IO);
    set_dtt1(DTT1_VRAM);

    /*
     * Supervisor accesses walk SRP and user accesses walk URP, chosen by
     * the function code and not by anything software says at the time.
     * URP starts pointing at the kernel's map as well, because nothing
     * runs in user mode yet and a root pointer aimed at nothing is a
     * double fault waiting for the first program.
     */
    set_srp(kernel_root);
    set_urp(kernel_root);
    pflusha();

    /*
     * The cliff.
     *
     * The instruction after this one has to be mapped or the machine is
     * gone with nothing to say why. Three things make that safe: the map
     * above is identity, so every address keeps its meaning; ITT0 covers
     * supervisor instruction fetch regardless of the tables; and the
     * supervisor stack is inside the region just mapped, which matters
     * more than it looks -- a fault taken while pushing a fault frame is
     * not an exception, it is the emulator aborting with DOUBLE MMU
     * FAULT and no output at all.
     */
    set_tc(TC_ENABLE);
    pflusha();

    enabled = (get_tc() & TC_ENABLE) != 0;
    if (!enabled) {
        kputln("vm: the MMU did not come on");
        halt();
    }
}

int vm_enabled(void)
{
    return enabled;
}

/* --- user address spaces -------------------------------------------- */

/*
 * TABLES ARE ALLOCATED ON DEMAND, in 512-byte slots.
 *
 * There used to be a comment here explaining that one page held
 * everything an address space needs -- root at 0x000, pointer table at
 * 0x200, eight page tables from 0x400 -- and that creating a space was
 * therefore one allocation and destroying it one free. That was true,
 * it was a good trade, and it is what limited the user area to 2 MB.
 *
 * 256 MB needs up to 1024 page tables, and a program that touches a
 * megabyte needs four of them. Allocating all of them would waste more
 * memory than the programs use. So:
 *
 *   - a table page is taken from the page allocator when one is needed
 *   - its FIRST 512-byte slot holds a link to the previous such page,
 *     so they form a list the address space can free later
 *   - the remaining seven slots are handed out to tables
 *
 * A root table and a pointer table are 512 bytes; a page table is 256.
 * Everything is given a 512-byte slot regardless, because the alignment
 * rules differ per level and one size removes the question.
 */
#define AS_SLOT         512
#define MAX_SPACES      32      /* one per task: TASK_MAX in task.h */

static struct addrspace spaces[MAX_SPACES];

/*
 * A 512-byte slot for a table, from this address space's own pages.
 * Returns a physical address, or 0 if memory is gone.
 */
static u32 as_table_alloc(struct addrspace *as)
{
    u32 t;

    if (!as->slot_page || as->slot_off + AS_SLOT > PAGE_SIZE) {
        u32 pg = pmm_alloc();

        if (!pg) {
            return 0;
        }
        /* pmm_alloc zeroes the page, so every slot starts empty. The
         * first one becomes the link and the rest are available. */
        *(u32 *)pg = as->tables;
        as->tables = pg;
        as->slot_page = pg;
        as->slot_off = AS_SLOT;
    }
    t = as->slot_page + as->slot_off;
    as->slot_off += AS_SLOT;
    return t;
}

/*
 * Find the page table covering `va`, building the path to it if asked.
 *
 * `create` is the whole difference between mapping and translating: a
 * map must be able to bring tables into existence, and a translate must
 * never do so -- otherwise reading a wild pointer would quietly
 * allocate the tables to describe it.
 */
static u32 as_pagetable(struct addrspace *as, u32 va, int create)
{
    u32 *root, *ptr, d;
    u32 ptr_pa, pt_pa;

    if (!as || va < USER_VA_BASE || va >= USER_VA_END) {
        return 0;
    }

    root = table(as->root);
    d = root[ROOT_INDEX(va)];
    if (!(d & UDT_RESIDENT)) {
        if (!create) {
            return 0;
        }
        ptr_pa = as_table_alloc(as);
        if (!ptr_pa) {
            return 0;
        }
        root[ROOT_INDEX(va)] = ptr_pa | UDT_RESIDENT;
    }
    ptr_pa = root[ROOT_INDEX(va)] & PTR_TABLE_MASK;

    ptr = table(ptr_pa);
    d = ptr[PTR_INDEX(va)];
    if (!(d & UDT_RESIDENT)) {
        if (!create) {
            return 0;
        }
        pt_pa = as_table_alloc(as);
        if (!pt_pa) {
            return 0;
        }
        ptr[PTR_INDEX(va)] = pt_pa | UDT_RESIDENT;
    }
    return ptr[PTR_INDEX(va)] & PAGE_TABLE_MASK;
}

struct addrspace *vm_create(void)
{
    struct addrspace *as = 0;
    int i;

    for (i = 0; i < MAX_SPACES; i++) {
        if (!spaces[i].used) {
            as = &spaces[i];
            break;
        }
    }
    if (!as) {
        return 0;
    }

    as->tables = 0;
    as->slot_page = 0;
    as->slot_off = 0;
    as->brk_start = 0;
    as->brk_cur = 0;

    as->root = as_table_alloc(as);
    if (!as->root) {
        return 0;
    }
    as->used = 1;

    /*
     * Nothing else is filled in. Every root entry is invalid until
     * something maps an address under it, which is what makes a stray
     * user pointer a fault rather than somebody else's data -- and, now
     * that the space is 256 MB, what keeps an address space that has
     * touched one page from costing a megabyte of tables.
     */
    return as;
}

u32 vm_map(struct addrspace *as, u32 va, u32 pa, int flags)
{
    u32 pt_pa = as_pagetable(as, va, 1);
    u32 *pt;

    if (!pt_pa) {
        return 0;
    }
    if (!pa) {
        pa = pmm_alloc();
        if (!pa) {
            return 0;
        }
    }

    pt = table(pt_pa);
    pt[PAGE_INDEX(va)] = (pa & PAGE_ADDR_MASK) | page_bits(flags | VM_USER);

    /*
     * The address space being changed may be the one currently loaded,
     * and a stale translation for this page may be sitting in the cache
     * from the program that had the address space before this one.
     */
    pflusha();
    return pa;
}

u32 vm_translate(struct addrspace *as, u32 va, int write)
{
    /*
     * Never creates. A translate that built tables would mean reading a
     * wild pointer quietly allocated the tables to describe it, and a
     * program could exhaust memory by dereferencing rubbish.
     */
    u32 pt_pa = as_pagetable(as, va, 0);
    u32 d;

    if (!pt_pa) {
        return 0;
    }
    d = table(pt_pa)[PAGE_INDEX(va)];

    if ((d & PDT_RESIDENT) == 0) {
        return 0;
    }
    if (write && (d & DESC_WP)) {
        return 0;
    }
    if (d & DESC_SUPER) {
        /* Not reachable today -- vm_map forces VM_USER -- but a
         * supervisor page is not a program's to read whatever asked. */
        return 0;
    }
    return (d & PAGE_ADDR_MASK) | (va & PAGE_MASK);
}

u32 vm_mapped_pages(struct addrspace *as)
{
    u32 n = 0, va;

    /*
     * Steps a page table at a time when there is no table, rather than
     * a page. Over 256 MB the difference is 1024 iterations against
     * 65536, and this is called by `free` on every prompt.
     */
    for (va = USER_VA_BASE; va < USER_VA_END; ) {
        u32 pt_pa = as_pagetable(as, va, 0);
        u32 i;

        if (!pt_pa) {
            va += PAGE_ENTRIES * PAGE_SIZE;
            continue;
        }
        for (i = 0; i < PAGE_ENTRIES; i++) {
            if (desc_owned(table(pt_pa)[i])) {
                n++;
            }
        }
        va += PAGE_ENTRIES * PAGE_SIZE;
    }
    return n;
}

int vm_is_mapped(struct addrspace *as, u32 va)
{
    u32 pt_pa = as_pagetable(as, va, 0);

    return pt_pa && desc_owned(table(pt_pa)[PAGE_INDEX(va)]);
}

int vm_protect(struct addrspace *as, u32 va, int flags)
{
    u32 pt_pa = as_pagetable(as, va, 0);
    u32 *pt, d, pa;

    if (!pt_pa) {
        return -1;
    }
    pt = table(pt_pa);
    d = pt[PAGE_INDEX(va)];
    if (!desc_owned(d)) {
        return -1;
    }
    pa = d & PAGE_ADDR_MASK;
    if (flags & VM_NONE) {
        pt[PAGE_INDEX(va)] = pa | DESC_SW_NONE;
    } else {
        pt[PAGE_INDEX(va)] = pa | page_bits(flags | VM_USER);
    }

    /* Without this the old descriptor stays in the ATC and the change
     * takes effect at some later, unrelated moment -- the hardest
     * failure in this tree to find. */
    pflusha();
    return 0;
}

void vm_unmap(struct addrspace *as, u32 va)
{
    u32 pt_pa = as_pagetable(as, va, 0);
    u32 *pt, d;

    if (!pt_pa) {
        return;
    }
    pt = table(pt_pa);
    d = pt[PAGE_INDEX(va)];
    if (!desc_owned(d)) {
        return;
    }
    pt[PAGE_INDEX(va)] = 0;
    pmm_free(d & PAGE_ADDR_MASK);

    /* Before anything can reuse that page, not after: a cached
     * translation would let the program keep writing to memory that now
     * belongs to somebody else. */
    pflusha();
}

/*
 * THE PROGRAM BREAK.
 *
 * The heap is the range [brk_start, brk_cur), mapped a page at a time
 * as it grows and unmapped as it shrinks. What is mapped is always
 * exactly [brk_start, PAGE_ALIGN_UP(brk_cur)), so the break itself can
 * sit anywhere inside a page and the page arithmetic takes care of it.
 *
 * The return convention is Linux's and it is not an accident: a request
 * that cannot be met returns the OLD break rather than an error. Every
 * malloc ever written for Linux detects failure by comparing what came
 * back with what it asked for, so any other convention would make every
 * one of them wrong on this machine.
 *
 * Growth refuses to pass over a page that is already mapped. Nothing
 * maps there today, but mmap will place things in the same gap, and a
 * heap that silently grew over a mapping would replace its pages.
 *
 * Shrinking does not zero the part of the last page left beyond the
 * break, and growing back over it returns whatever the program left
 * there. That is Linux's behaviour as well, and it discloses nothing:
 * the data was the program's own. Every NEW page arrives zeroed,
 * because pmm_alloc zeroes, and that is the guarantee that matters --
 * another program's old memory must never appear in this one.
 */
u32 vm_brk(struct addrspace *as, u32 addr)
{
    u32 old_end, new_end, va;

    if (!as) {
        return 0;               /* a kernel task has no heap */
    }
    if (addr < as->brk_start || addr > USER_BRK_LIMIT) {
        return as->brk_cur;     /* brk(0) lands here, as it should */
    }

    old_end = PAGE_ALIGN_UP(as->brk_cur);
    new_end = PAGE_ALIGN_UP(addr);

    if (new_end > old_end) {
        u32 pages = (new_end - old_end) / PAGE_SIZE;

        /*
         * Refused up front when the memory plainly is not there: the
         * pages, plus a page of tables per 448 pages (seven 64-page
         * tables to a table page), plus two for the pointer tables.
         * Trying and rolling back would work too, but the tables built
         * on the way would stay with the address space until it died,
         * so a failed request would cost the machine memory for nothing.
         */
        if (pmm_available() < pages + pages / 448 + 2) {
            return as->brk_cur;
        }
        for (va = old_end; va < new_end; va += PAGE_SIZE) {
            if (vm_is_mapped(as, va)) {
                return as->brk_cur;
            }
        }
        for (va = old_end; va < new_end; va += PAGE_SIZE) {
            if (!vm_map(as, va, 0, VM_USER | VM_WRITE)) {
                /* All or nothing: a half-grown heap is a break that
                 * does not match what is mapped. */
                while (va > old_end) {
                    va -= PAGE_SIZE;
                    vm_unmap(as, va);
                }
                return as->brk_cur;
            }
        }
    } else {
        for (va = new_end; va < old_end; va += PAGE_SIZE) {
            vm_unmap(as, va);
        }
    }

    as->brk_cur = addr;
    return addr;
}

struct addrspace *vm_clone(struct addrspace *src)
{
    struct addrspace *as;
    u32 pages = vm_mapped_pages(src), va;

    /*
     * Refused before anything is copied if it plainly cannot fit: the
     * pages, and a page of tables per 448 of them, as brk reckons it.
     */
    if (pmm_available() < pages + pages / 448 + 4) {
        return 0;
    }
    as = vm_create();
    if (!as) {
        return 0;
    }

    for (va = USER_VA_BASE; va < USER_VA_END; ) {
        u32 pt_pa = as_pagetable(src, va, 0);
        u32 i;

        if (!pt_pa) {
            va += PAGE_ENTRIES * PAGE_SIZE;
            continue;
        }
        for (i = 0; i < PAGE_ENTRIES; i++, va += PAGE_SIZE) {
            u32 d = table(pt_pa)[i];
            u32 dst_pt, pa;

            if (!desc_owned(d)) {
                continue;
            }
            pa = pmm_alloc();
            dst_pt = as_pagetable(as, va, 1);
            if (!pa || !dst_pt) {
                if (pa) {
                    pmm_free(pa);
                }
                vm_destroy(as);
                return 0;
            }
            /* The kernel sees all of RAM at its own address, so a page
             * copy is a memcpy between physical addresses. */
            memcpy((void *)pa, (void *)(d & PAGE_ADDR_MASK), PAGE_SIZE);
            /* Same protection, same everything, different page -- a
             * PROT_NONE page stays one. */
            table(dst_pt)[PAGE_INDEX(va)] = pa | (d & ~PAGE_ADDR_MASK);
        }
    }
    as->brk_start = src->brk_start;
    as->brk_cur = src->brk_cur;
    pflusha();
    return as;
}

void vm_destroy(struct addrspace *as)
{
    u32 va, pg;

    if (!as || !as->used) {
        return;
    }

    /* Every page it was given. Stepping by page table, so an address
     * space that touched one megabyte does not cost 65536 iterations
     * to tear down. */
    for (va = USER_VA_BASE; va < USER_VA_END; ) {
        u32 pt_pa = as_pagetable(as, va, 0);
        u32 i;

        if (!pt_pa) {
            va += PAGE_ENTRIES * PAGE_SIZE;
            continue;
        }
        for (i = 0; i < PAGE_ENTRIES; i++) {
            u32 d = table(pt_pa)[i];

            if (desc_owned(d)) {
                pmm_free(d & PAGE_ADDR_MASK);
                table(pt_pa)[i] = 0;
            }
        }
        va += PAGE_ENTRIES * PAGE_SIZE;
    }

    /*
     * Then the pages the tables themselves lived in, off the list.
     * Which pages were tables is not recoverable from the tables, which
     * is exactly why the list exists.
     */
    pg = as->tables;
    while (pg) {
        u32 next = *(u32 *)pg;

        pmm_free(pg);
        pg = next;
    }

    as->used = 0;
    as->tables = 0;
    as->slot_page = 0;
    as->slot_off = 0;
    as->brk_start = 0;
    as->brk_cur = 0;
    as->root = 0;

    /* Whatever was cached for those addresses now refers to pages that
     * belong to nobody. */
    pflusha();
}

void vm_kernel_present(u32 va, int present)
{
    u32 *root = table(kernel_root);
    u32 rd = root[ROOT_INDEX(va)];
    u32 *ptr, pd, *pt;

    if ((rd & UDT_RESIDENT) == 0) {
        return;
    }
    ptr = table(rd & PTR_TABLE_MASK);
    pd = ptr[PTR_INDEX(va)];
    if ((pd & UDT_RESIDENT) == 0) {
        return;
    }
    pt = table(pd & PAGE_TABLE_MASK);

    if (present) {
        pt[PAGE_INDEX(va)] = (va & PAGE_ADDR_MASK) | page_bits(VM_WRITE);
    } else {
        pt[PAGE_INDEX(va)] = 0;
    }
    pflusha();
}

void vm_switch(struct addrspace *as)
{
    set_urp(as ? as->root : kernel_root);

    /*
     * The 68040 has no address space identifier, so there is nothing to
     * tell one program's translations from another's: the whole cache
     * goes. That is the cost of a context switch here and it is worth
     * knowing before anybody wonders why switching is not free.
     */
    pflusha();
}
