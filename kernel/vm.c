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
#include "cache.h"
#include "pmm.h"
#include "swap.h"
#include "textcache.h"
#include "console.h"
#include "errno.h"
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

/*
 * DEMAND PAGING's descriptors (task 21). The first two are INVALID
 * descriptors, so the MMU faults on the page and vm_fault() decides;
 * the third is a bit on a RESIDENT one.
 *
 *   SW_LAZY  nothing yet: the first touch makes a zeroed page
 *   SW_SWAP  in the swap file, at the slot in bits 31..12
 *   SW_COW   resident but write-protected because another address
 *            space holds it too; a write copies it (fork's pages)
 *
 * DESC_WP on a lazy or swapped descriptor means what it does on a
 * resident one -- read-only -- so the protection survives the page
 * being away. SW_NONE on either means PROT_NONE. SW_COW shares bit 11
 * with SW_NONE, which is safe because one is only ever looked at on a
 * resident descriptor and the other only on an invalid one.
 */
#define DESC_SW_LAZY    0x400
#define DESC_SW_SWAP    0x200
#define DESC_SW_COW     0x800
#define DESC_USED       0x008           /* set by the MMU on access     */

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

/* Does this descriptor occupy its address -- a page, or a promise of
 * one? What mmap and brk ask before putting something there. */
static int desc_owned(u32 d)
{
    return (d & PDT_RESIDENT) ||
           (d & (DESC_SW_NONE | DESC_SW_LAZY | DESC_SW_SWAP));
}

/* Does it hold a physical page? Resident, or PROT_NONE with its page. */
static int desc_has_page(u32 d)
{
    return (d & PDT_RESIDENT) ||
           ((d & DESC_SW_NONE) && !(d & (DESC_SW_LAZY | DESC_SW_SWAP)));
}

/* Give back whatever the descriptor holds: a page, a swap slot, or
 * nothing at all for a lazy one. */
static void desc_release(u32 d)
{
    if (desc_has_page(d)) {
        pmm_free(d & PAGE_ADDR_MASK);
    } else if (d & DESC_SW_SWAP) {
        swap_free(d >> PAGE_SHIFT);
    }
}

static struct vm_stats stats;

void vm_stats(struct vm_stats *out)
{
    *out = stats;
}

/*
 * Pages kept back from programs for the kernel's own use -- page
 * tables, a task's kernel stack, a socket's buffers -- which take pages
 * straight from pmm and must not find them all gone to a program that
 * touched too much.
 */
#define RESERVE_PAGES   24
#define RECLAIM_BATCH   32

/* A page for a program: reclaiming first if memory is short. */
static u32 page_alloc_user(void)
{
    if (pmm_available() <= RESERVE_PAGES + 1) {
        vm_reclaim(RECLAIM_BATCH);
    }
    if (pmm_available() <= RESERVE_PAGES) {
        return 0;
    }
    return pmm_alloc();
}

int vm_commit_ok(u32 pages)
{
    struct swapstats s;
    u32 have = pmm_available() + textcache_idle();

    swap_stats(&s);
    have += s.slots - s.used;
    return pages + RESERVE_PAGES <= have;
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

static void vm_table_nocache(u32 pa);

static u32 ktable_alloc(void)
{
    u32 t;

    if (ktbl_page == 0 || ktbl_off + 512 > PAGE_SIZE) {
        ktbl_page = pmm_alloc();
        if (!ktbl_page) {
            return 0;
        }
        ktbl_off = 0;
        /* Tables are not cachable; see kset_cachemode. Before the map
         * exists this does nothing and the boot sweep catches it. */
        vm_table_nocache(ktbl_page);
    }
    t = ktbl_page + ktbl_off;
    ktbl_off += 512;
    return t;
}

/*
 * THE CACHE MODE OF ONE PAGE, in the kernel's own identity map.
 *
 * Only ever used to make a page NON-CACHABLE, and only for one kind of
 * page: memory holding TRANSLATION TABLES. The reason is a property of
 * the 68040 rather than a choice.
 *
 * The MMU's table search reads descriptors from memory DIRECTLY and
 * writes the U and M bits back the same way. It does not go through
 * the data cache and does not snoop it. So with the data cache on and
 * tables cachable, two things go wrong and both are silent:
 *
 *   - the kernel writes a descriptor, it sits dirty in the cache, and
 *     the MMU walks the STALE copy still in memory;
 *   - the MMU sets U or M in memory, the cache later writes back its
 *     own older copy of that line, and the bits are lost -- which for
 *     M means a modified page is evicted as clean and the program's
 *     writes are thrown away.
 *
 * Linux/m68k does exactly this for the same reason: mmu_page_ctor()
 * calls nocache_page() on every page-table page for the 040.
 *
 * Returns 0 if the mode was set. Before the MMU is on this still works
 * -- the map is being built in ordinary memory and the descriptors are
 * just words -- which is what lets the tables built during boot be
 * marked before the caches are ever enabled.
 */
static int kset_cachemode(u32 pa, u32 cm)
{
    u32 va = pa;                        /* the kernel map is identity */
    u32 *root = table(kernel_root);
    u32 rd, pd, *ptr, *pt;
    int idx;

    if (!kernel_root) {
        return -1;
    }
    rd = root[ROOT_INDEX(va)];
    if ((rd & UDT_RESIDENT) == 0) {
        return -1;
    }
    ptr = table(rd & PTR_TABLE_MASK);
    pd = ptr[PTR_INDEX(va)];
    if ((pd & UDT_RESIDENT) == 0) {
        return -1;
    }
    pt = table(pd & PAGE_TABLE_MASK);
    idx = (int)PAGE_INDEX(va);
    if ((pt[idx] & PDT_RESIDENT) == 0) {
        return -1;
    }
    pt[idx] = (pt[idx] & ~(u32)DESC_CM_NC) | cm;
    /*
     * The old mode may be cached in the ATC, and with a cachable->NC
     * change any dirty lines for the page have to go out before the
     * hardware stops looking at the cache for it. cpusha is a superset
     * of the push this needs and costs nothing while the caches are
     * off.
     */
    cache_flush_all();
    pflusha();
    return 0;
}

/*
 * Mark a page that holds translation tables non-cachable. Safe to call
 * before the MMU is on and safe to call twice.
 */
void vm_table_nocache(u32 pa)
{
    (void)kset_cachemode(pa & ~(u32)PAGE_MASK, DESC_CM_NC);
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

    /*
     * THE TABLES BUILT ON THE WAY HERE, marked now.
     *
     * vm_table_nocache() does nothing while the map is still being
     * built -- there is no descriptor to change yet -- so every table
     * page allocated during the loop above is still cachable. They are
     * swept here, once, before the caches are turned on, which is the
     * only moment at which "every table page" is a finite list that
     * can be walked from the root.
     *
     * Every page table reachable from the kernel root, plus the root
     * and the pointer tables themselves. ktable_alloc hands out 512
     * bytes at a time, so several tables share a page and marking the
     * page twice is harmless.
     */
    {
        u32 *root = table(kernel_root);
        int ri, pi;

        vm_table_nocache(kernel_root);
        for (ri = 0; ri < ROOT_ENTRIES; ri++) {
            u32 rd = root[ri];
            u32 *ptr;

            if ((rd & UDT_RESIDENT) == 0) {
                continue;
            }
            vm_table_nocache(rd & PTR_TABLE_MASK);
            ptr = table(rd & PTR_TABLE_MASK);
            for (pi = 0; pi < PTR_ENTRIES; pi++) {
                u32 pd = ptr[pi];

                if ((pd & UDT_RESIDENT) == 0) {
                    continue;
                }
                vm_table_nocache(pd & PAGE_TABLE_MASK);
            }
        }
    }

    /*
     * And now the caches. Everything cache_enable() requires is true:
     * the MMU is on, the device and video windows are CM_NC through
     * DTT0 and DTT1, and every translation table is non-cachable.
     *
     * None of it can be observed here -- QEMU has no cache model -- so
     * this is the one part of the memory system that is correct by
     * inspection rather than by test. See the head of cache.c.
     */
    cache_enable();
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
#define MAX_SPACES      64      /* one per task: TASK_MAX in task.h */

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
        /* Written BEFORE the mode changes: the link word above goes
         * through the cache, and the push inside kset_cachemode is
         * what gets it to memory where the MMU will look. */
        vm_table_nocache(pg);
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
    as->refs = 1;               /* the task that asked for it */
    as->busy = 1;               /* until vm_ready(): see vm.h */

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
        pa = page_alloc_user();
        if (!pa) {
            return 0;
        }
        /* The tables may have been where the page came from, if
         * reclaim ran: look again. */
        pt_pa = as_pagetable(as, va, 1);
        if (!pt_pa) {
            pmm_free(pa);
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

int vm_may_write(struct addrspace *as, u32 va)
{
    u32 pt_pa = as_pagetable(as, va, 0);
    u32 d;

    if (!pt_pa) {
        return 0;
    }
    d = table(pt_pa)[PAGE_INDEX(va)];
    if (d & PDT_RESIDENT) {
        return !(d & DESC_SUPER) && (!(d & DESC_WP) || (d & DESC_SW_COW));
    }
    return (d & (DESC_SW_LAZY | DESC_SW_SWAP)) &&
           !(d & (DESC_WP | DESC_SW_NONE));
}

void vm_ready(struct addrspace *as)
{
    if (as) {
        as->busy = 0;
    }
}

int vm_map_lazy(struct addrspace *as, u32 va, int flags)
{
    u32 pt_pa = as_pagetable(as, va, 1);
    u32 d = DESC_SW_LAZY;

    if (!pt_pa) {
        return -1;
    }
    if (!(flags & VM_WRITE)) {
        d |= DESC_WP;
    }
    if (flags & VM_NONE) {
        d |= DESC_SW_NONE;
    }
    /* An invalid descriptor replacing an invalid one: nothing cached to
     * flush. The callers unmap first when there was a page. */
    table(pt_pa)[PAGE_INDEX(va)] = d;
    return 0;
}

/*
 * THE FAULT PATH. Every way a mapped page can be absent, made present:
 *
 *   resident, write-protected, SW_COW   copy it if anyone else holds
 *                                       it, then make it writable
 *   SW_LAZY                             a zeroed page (pmm zeroes)
 *   SW_SWAP                             read back from the swap file
 *
 * and a resident, accessible page is a translation the MMU had cached
 * from before it changed -- flushed, and the access tried again.
 * Everything else is a real fault. The page made is marked USED, so
 * that reclaim does not take it back before the instruction that
 * wanted it has run again.
 */
int vm_fault(struct addrspace *as, u32 va, int write)
{
    u32 pt_pa = as_pagetable(as, va, 0);
    u32 *pt, d, pa;
    int idx = (int)PAGE_INDEX(va);

    if (!pt_pa) {
        return -EFAULT;
    }
    pt = table(pt_pa);
    d = pt[idx];

    if (d & PDT_RESIDENT) {
        if (d & DESC_SUPER) {
            return -EFAULT;
        }
        if (!write || !(d & DESC_WP)) {
            pflusha();
            return VM_FAULT_NOCHANGE;
        }
        if (!(d & DESC_SW_COW)) {
            return -EFAULT;             /* read-only, and meant to be */
        }
        pa = d & PAGE_ADDR_MASK;
        if (pmm_refcount(pa) > 1) {
            u32 copy = page_alloc_user();       /* may sleep */

            if (!copy) {
                return -ENOMEM;
            }
            /*
             * While that slept the other holder may have gone, leaving
             * this the only one -- and then reclaim may have taken it.
             * If the descriptor is not what it was, start again.
             */
            if (pt[idx] != d) {
                pmm_free(copy);
                return 0;
            }
            memcpy((void *)copy, (void *)pa, PAGE_SIZE);
            pmm_free(pa);
            pa = copy;
        }
        pt[idx] = pa | page_bits(VM_USER | VM_WRITE) | DESC_USED;
        stats.faults_cow++;
        pflusha();
        return 0;
    }

    if ((d & DESC_SW_NONE) || !(d & (DESC_SW_LAZY | DESC_SW_SWAP))) {
        return -EFAULT;                 /* PROT_NONE, or nothing there */
    }
    if (write && (d & DESC_WP)) {
        return -EFAULT;
    }
    if (d & DESC_SW_SWAP) {
        swap_wait_idle(d >> PAGE_SHIFT);    /* still being written out */
    }
    pa = page_alloc_user();                 /* may sleep, reclaiming */
    if (!pa) {
        return -ENOMEM;
    }
    if (pt[idx] != d) {
        /* Something else resolved it while this slept -- swapoff. */
        pmm_free(pa);
        return 0;
    }
    if (d & DESC_SW_SWAP) {
        u32 slot = d >> PAGE_SHIFT;

        if (swap_read(slot, (void *)pa) < 0) {  /* sleeps */
            pmm_free(pa);
            return -EFAULT;
        }
        if (pt[idx] != d) {
            pmm_free(pa);
            return 0;
        }
        swap_free(slot);                /* this space's hold on it */
        stats.faults_swapin++;
    } else {
        stats.faults_zero++;
    }
    pt[idx] = pa | page_bits(VM_USER | ((d & DESC_WP) ? 0 : VM_WRITE)) |
              DESC_USED;
    pflusha();
    return 0;
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
        /* Pages in memory: a lazy page is not one yet, and a swapped
         * one is not one any more. */
        for (i = 0; i < PAGE_ENTRIES; i++) {
            if (desc_has_page(table(pt_pa)[i])) {
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

    /* Lazy or swapped: no page to change, only what it will be when it
     * comes -- the bits vm_fault() reads. */
    if (!desc_has_page(d)) {
        u32 nd = d & ~(DESC_WP | DESC_SW_NONE);

        if (!(flags & VM_WRITE)) {
            nd |= DESC_WP;
        }
        if (flags & VM_NONE) {
            nd |= DESC_SW_NONE;
        }
        pt[PAGE_INDEX(va)] = nd;
        return 0;
    }
    pa = d & PAGE_ADDR_MASK;

    /*
     * COPY ON WRITE, done at the only moment write can be granted. A
     * page with more than one holder is shared -- a library's text from
     * textcache.c, or a read-only page a fork left in both processes --
     * and letting one of them write to it would change it under all the
     * others. So a writable mapping of it gets a page of its own first.
     * Nothing else in the tree maps a page writable that it did not
     * just allocate, which is why this one check is enough.
     */
    if ((flags & VM_WRITE) && !(flags & VM_NONE) && pmm_refcount(pa) > 1) {
        u32 copy = page_alloc_user();           /* may sleep */

        if (!copy) {
            return -1;
        }
        if (pt[PAGE_INDEX(va)] != d) {
            /* Reclaim took it while that slept: do it all again. */
            pmm_free(copy);
            return vm_protect(as, va, flags);
        }
        memcpy((void *)copy, (void *)pa, PAGE_SIZE);
        pmm_free(pa);                   /* one holder fewer */
        pa = copy;
    }

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
    desc_release(d);

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
        /*
         * The pages are lazy (demand paging, task 21): nothing is taken
         * until they are touched, so what is checked is whether they
         * could be -- memory and swap together -- plus a page of
         * tables per 448 of them, which ARE taken now.
         */
        if (!vm_commit_ok(pages) || pmm_available() < pages / 448 + 2) {
            return as->brk_cur;
        }
        for (va = old_end; va < new_end; va += PAGE_SIZE) {
            if (vm_is_mapped(as, va)) {
                return as->brk_cur;
            }
        }
        for (va = old_end; va < new_end; va += PAGE_SIZE) {
            if (vm_map_lazy(as, va, VM_USER | VM_WRITE) < 0) {
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
     * COPY ON WRITE (task 21): nothing is copied here, so all a fork
     * needs up front is the tables -- a page of them per 448 pages, as
     * brk reckons it.
     */
    if (pmm_available() < pages / 448 + 4) {
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
            u32 *sp = &table(pt_pa)[i];
            u32 d = *sp;
            u32 dst_pt, pa;

            if (!desc_owned(d)) {
                continue;
            }
            dst_pt = as_pagetable(as, va, 1);
            if (!dst_pt) {
                vm_destroy(as);
                return 0;
            }

            /* Nothing yet: the child gets the same promise. */
            if (d & DESC_SW_LAZY) {
                table(dst_pt)[PAGE_INDEX(va)] = d;
                continue;
            }
            /* In the swap file: both hold the slot until one of them
             * brings it back (vm_fault gives up its own hold). */
            if (d & DESC_SW_SWAP) {
                if (swap_ref(d >> PAGE_SHIFT) < 0) {
                    vm_destroy(as);
                    return 0;
                }
                table(dst_pt)[PAGE_INDEX(va)] = d;
                continue;
            }

            pa = d & PAGE_ADDR_MASK;
            /* Not RAM at all -- a device's memory, /dev/fb0 mapped: the
             * same pages in both, as they are the device's. */
            if ((d & PDT_RESIDENT) && pmm_refcount(pa) == 0) {
                table(dst_pt)[PAGE_INDEX(va)] = d;
                continue;
            }
            if (pmm_ref(pa)) {
                /*
                 * SHARED. A page neither side can write stays as it
                 * is in both. A writable one becomes read-only in both
                 * and marked SW_COW, so that the first write by either
                 * faults, and vm_fault() gives the writer its own copy
                 * -- or, if the other has gone by then, simply makes
                 * the page writable again.
                 */
                if ((d & PDT_RESIDENT) && (!(d & DESC_WP) || (d & DESC_SW_COW))) {
                    d = (d | DESC_WP | DESC_SW_COW) & ~DESC_USED;
                    *sp = d;
                }
                table(dst_pt)[PAGE_INDEX(va)] = d;
                continue;
            }

            /* 65535 holders already: a copy of its own, then. */
            pa = page_alloc_user();
            if (!pa) {
                vm_destroy(as);
                return 0;
            }
            memcpy((void *)pa, (void *)(d & PAGE_ADDR_MASK), PAGE_SIZE);
            table(dst_pt)[PAGE_INDEX(va)] = pa | (d & ~(PAGE_ADDR_MASK | DESC_SW_COW));
        }
    }
    as->brk_start = src->brk_start;
    as->brk_cur = src->brk_cur;
    as->busy = 0;
    /* The parent's writable pages just became read-only: its cached
     * translations still say writable. */
    pflusha();
    return as;
}

struct addrspace *vm_share(struct addrspace *as)
{
    if (as) {
        as->refs++;
    }
    return as;
}

/*
 * One task fewer is using this space, and everything in it goes when
 * the last one does. A thread calling this is letting go; the process's
 * memory outlives it, because its siblings are still running in it.
 */
void vm_destroy(struct addrspace *as)
{
    u32 va, pg;

    if (!as || !as->used) {
        return;
    }
    if (as->refs > 1) {
        as->refs--;
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
                desc_release(d);
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

/*
 * RECLAIM: the clock algorithm, over every address space's pages.
 *
 * The hand walks (space, address) positions and remembers where it
 * stopped. A resident page the MMU has marked USED since the hand last
 * passed has its mark cleared and is left; one without the mark has
 * not been touched for a whole turn of the hand, and is written to the
 * swap file and freed. Two turns at most, so a machine where every page
 * is in use gives up rather than spinning.
 *
 * A page with ONE holder is written out and freed. A page shared
 * copy-on-write after a fork has two, and is taken from ONE space at a
 * time: written out, that space's descriptor pointed at the slot, and
 * its hold given up. That frees nothing yet, but leaves the other space
 * the only holder, and the hand frees it the next time round. Without
 * that, a process that forked held all its memory where nothing could
 * reach it -- the parent of a fork ran out with swap to spare.
 *
 * Other shared pages are left alone. A library's text belongs to the
 * file cache and is read-only; and a page PINNED by a system call that
 * is reading into it must stay where the kernel was told it is. The
 * difference is the SW_COW mark: a pinned page is writable, which a
 * copy-on-write one never is.
 *
 * Called only where a page is about to be given to a program (see
 * page_alloc_user), never from inside pmm_alloc(): the kernel takes
 * pages for its own use all over, at moments when an address space's
 * tables may be half built, and nothing there expects its pages to move.
 */
static u32 hand_space, hand_va = USER_VA_BASE;

/*
 * Write one page out and free it.
 *
 * The page leaves its owner's map FIRST, and only then is written: the
 * write SLEEPS (the disk is interrupt-driven), and while it does the
 * owner may run -- a page still mapped could be changed half way through
 * being written, and the change lost when the page is freed -- and
 * another task's reclaim may run, and must not find the page still
 * there to take a second time. The slot is marked busy for the length of
 * the write, so a fault on the page waits for it to finish.
 *
 * Nothing here touches `pt` after the write: the address space may have
 * gone while this slept -- its owner exiting -- taking its tables with
 * it. The page itself is this function's until it frees it.
 */
static int evict(u32 *pt, int idx)
{
    u32 d = pt[idx], pa = d & PAGE_ADDR_MASK, slot;
    int ro = (d & DESC_WP) && !(d & DESC_SW_COW);

    slot = swap_alloc();
    if (slot == SWAP_NONE) {
        return -1;                      /* swap is full */
    }
    swap_set_busy(slot, 1);
    pt[idx] = (slot << PAGE_SHIFT) | DESC_SW_SWAP | (ro ? DESC_WP : 0);
    pflusha();
    if (swap_write(slot, (void *)pa) < 0) {
        /* The page is lost with the disk; say so rather than hide it.
         * Its owner will read back whatever the slot holds. */
        kputs("\nswap: a page could not be written out\n");
    }
    swap_set_busy(slot, 0);
    pmm_free(pa);
    stats.evicted++;
    return 0;
}

u32 vm_reclaim(u32 want)
{
    u32 freed = textcache_shrink(want);
    u32 steps, limit;

    if (freed >= want || !swap_is_on()) {
        return freed;
    }

    /* Two full turns: every possible position, twice. */
    limit = 2 * MAX_SPACES * (USER_VA_SIZE / PAGE_SIZE);
    for (steps = 0; steps < limit && freed < want; ) {
        struct addrspace *as = &spaces[hand_space];
        u32 pt_pa;

        if (!as->used || as->busy || hand_va >= USER_VA_END) {
            hand_space = (hand_space + 1) % MAX_SPACES;
            hand_va = USER_VA_BASE;
            steps += (USER_VA_SIZE / PAGE_SIZE);
            continue;
        }
        pt_pa = as_pagetable(as, hand_va, 0);
        if (!pt_pa) {
            /* No table here: skip the 64 pages it would have covered. */
            u32 next = (hand_va & ~(PAGE_ENTRIES * PAGE_SIZE - 1)) +
                       PAGE_ENTRIES * PAGE_SIZE;

            steps += (next - hand_va) / PAGE_SIZE;
            hand_va = next;
            continue;
        }
        {
            u32 *pt = table(pt_pa);
            int idx = (int)PAGE_INDEX(hand_va);
            u32 d = pt[idx];

            u32 refs = (d & PDT_RESIDENT) ? pmm_refcount(d & PAGE_ADDR_MASK) : 0;

            if ((d & PDT_RESIDENT) && !(d & DESC_SUPER) &&
                (refs == 1 || (d & DESC_SW_COW))) {
                if (d & DESC_USED) {
                    pt[idx] = d & ~DESC_USED;
                } else if (evict(pt, idx) == 0) {
                    if (refs == 1) {
                        freed++;        /* a hold given up, not a page */
                    }
                } else {
                    break;              /* swap full or failing */
                }
            }
        }
        hand_va += PAGE_SIZE;
        steps++;
    }
    /* The USED marks cleared and the pages taken may be cached in the
     * ATC as they were: gone before any program runs again. */
    pflusha();
    return freed;
}

int vm_swapoff(void)
{
    int i;

    for (i = 0; i < MAX_SPACES; i++) {
        struct addrspace *as = &spaces[i];
        u32 va;

        if (!as->used) {
            continue;
        }
        for (va = USER_VA_BASE; va < USER_VA_END; ) {
            u32 pt_pa = as_pagetable(as, va, 0);
            u32 k;

            if (!pt_pa) {
                va += PAGE_ENTRIES * PAGE_SIZE;
                continue;
            }
            for (k = 0; k < PAGE_ENTRIES; k++, va += PAGE_SIZE) {
                u32 d = table(pt_pa)[k], pa, slot;

                if (!(d & DESC_SW_SWAP)) {
                    continue;
                }
                /* Straight from pmm: reclaiming now would only send
                 * something else out to the file being emptied. */
                pa = pmm_available() > RESERVE_PAGES ? pmm_alloc() : 0;
                if (!pa) {
                    return -ENOMEM;
                }
                slot = d >> PAGE_SHIFT;
                swap_wait_idle(slot);
                if (swap_read(slot, (void *)pa) < 0) {  /* sleeps */
                    pmm_free(pa);
                    return -EIO;
                }
                /*
                 * The read slept: the space may have gone, or its owner
                 * faulted the page in meanwhile. Only a descriptor that
                 * is still exactly what was read is replaced.
                 */
                if (!as->used || as_pagetable(as, va, 0) != pt_pa ||
                    table(pt_pa)[k] != d) {
                    pmm_free(pa);
                    continue;
                }
                swap_free(slot);
                if (d & DESC_SW_NONE) {
                    table(pt_pa)[k] = pa | DESC_SW_NONE;
                } else {
                    table(pt_pa)[k] = pa | page_bits(VM_USER |
                                        ((d & DESC_WP) ? 0 : VM_WRITE));
                }
            }
        }
    }
    pflusha();
    return 0;
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
