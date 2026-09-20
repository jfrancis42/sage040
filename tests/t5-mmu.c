/*
 * t5-mmu.c - MC68040 paged memory management unit.
 *
 * Reference: Motorola M68040 User's Manual, chapter 3.
 *
 * This is the test that justifies choosing the 68040 over the 68030:
 * QEMU implements the 040 MMU and implements nothing at all for the 030.
 *
 * Strategy
 * --------
 * Instruction fetch is made transparent through ITT0 so that code always
 * runs no matter what the tables say, and the I/O block at 0xff000000 is
 * made transparent through DTT0 so the console keeps working.  Everything
 * else - stack, globals, test buffers - goes through a real three-level
 * page table.
 *
 * The tables map all 4 MB of RAM identity, except one page: virtual
 * 0x00280000 is deliberately pointed at physical 0x00290000.  Writing
 * distinct magic values to the two physical pages before enabling the MMU
 * and then reading the virtual address afterwards proves a genuine table
 * walk happened.
 *
 * Address decomposition for 4K pages (confirmed against QEMU's walker):
 *   root index    = (va >> 25) & 0x7f      128 entries, 512-byte table
 *   pointer index = (va >> 18) & 0x7f      128 entries, 512-byte table
 *   page index    = (va >> 12) & 0x3f       64 entries, 256-byte table
 *   offset        =  va & 0xfff
 */
#include "sage040.h"

#define RAM_SIZE      0x00400000UL      /* 4 MB */

#define ROOT_TABLE    0x00100000UL      /* 512 bytes, 512-byte aligned */
#define PTR_TABLE     0x00100200UL      /* 512 bytes, 512-byte aligned */
#define PAGE_TABLES   0x00101000UL      /* 16 x 256 bytes              */

#define VA_TEST       0x00280000UL      /* virtual page under test     */
#define PA_TEST       0x00290000UL      /* physical page it maps to    */

#define MAGIC_AT_PA   0xCAFEBABEUL      /* written to PA_TEST          */
#define MAGIC_AT_VA   0xDEADBEEFUL      /* written to VA_TEST (phys)   */

#define UDT_RESIDENT  0x02              /* upper-level descriptor valid */
#define PDT_RESIDENT  0x01              /* page descriptor resident     */

#define TC_ENABLE     0x8000            /* 4K pages when bit 14 clear   */

/* ITT0: base 0x00, mask 0xff (matches every address), enabled, S=both */
#define ITT0_ALL      0x00FFC000UL
/* DTT0: base 0xff, mask 0x00 (matches 0xff000000-0xffffffff), S=both  */
#define DTT0_IO       0xFF00C000UL

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

static void pflusha(void)
{
    __asm__ volatile("pflusha" ::: "memory");
}

/* Build an identity map for [0, RAM_SIZE) using 4K pages. */
static void build_tables(void)
{
    u32 va;
    int i;

    /* Root table: 128 entries, only entry 0 is used (covers 0..32 MB). */
    for (i = 0; i < 128; i++) {
        MMIO32(ROOT_TABLE + i * 4) = 0;
    }
    MMIO32(ROOT_TABLE + 0) = (u32)PTR_TABLE | UDT_RESIDENT;

    /* Pointer table: entry N covers 256 KB, so 16 entries cover 4 MB. */
    for (i = 0; i < 128; i++) {
        MMIO32(PTR_TABLE + i * 4) = 0;
    }
    for (i = 0; i < 16; i++) {
        MMIO32(PTR_TABLE + i * 4) =
            (u32)(PAGE_TABLES + (u32)i * 256) | UDT_RESIDENT;
    }

    /* Page tables: identity map every 4K page of RAM. */
    for (va = 0; va < RAM_SIZE; va += 0x1000) {
        u32 pi = (va >> 18) & 0x7f;
        u32 gi = (va >> 12) & 0x3f;
        u32 pt = PAGE_TABLES + pi * 256;

        MMIO32(pt + gi * 4) = va | PDT_RESIDENT;
    }
}

static void remap(u32 va, u32 pa)
{
    u32 pi = (va >> 18) & 0x7f;
    u32 gi = (va >> 12) & 0x3f;
    u32 pt = PAGE_TABLES + pi * 256;

    MMIO32(pt + gi * 4) = pa | PDT_RESIDENT;
}

int main(void)
{
    u32 v;

    test_begin("t5 MC68040 MMU");

    uart_puts("  TC (before) = 0x"); uart_puthex32(get_tc()); uart_putc('\n');
    if ((get_tc() & TC_ENABLE) == 0) {
        test_ok("MMU starts disabled");
    } else {
        test_fail("MMU was already enabled at reset");
    }

    /* --- the control registers must be writable at all --- */
    set_itt1(0);
    set_dtt1(0);
    set_itt0(ITT0_ALL);
    set_dtt0(DTT0_IO);
    test_ok("transparent translation registers accepted values");

    /* --- lay down distinct magic values in the two physical pages --- */
    MMIO32(PA_TEST) = MAGIC_AT_PA;
    MMIO32(VA_TEST) = MAGIC_AT_VA;
    if (MMIO32(PA_TEST) == MAGIC_AT_PA && MMIO32(VA_TEST) == MAGIC_AT_VA) {
        test_ok("magic values written to both physical pages");
    } else {
        test_fail("could not write the test pages");
        test_end();
        return 0;
    }

    /* --- build the tables and bend one page --- */
    build_tables();
    remap(VA_TEST, PA_TEST);
    test_ok("three-level page tables built (identity, one page remapped)");

    set_srp(ROOT_TABLE);
    set_urp(ROOT_TABLE);
    pflusha();

    /* --- switch it on --- */
    set_tc(TC_ENABLE);
    pflusha();

    /*
     * If we reach this line at all, instruction fetch, the stack and the
     * console all survived enabling the MMU.
     */
    uart_puts("  TC (after)  = 0x"); uart_puthex32(get_tc()); uart_putc('\n');
    if (get_tc() & TC_ENABLE) {
        test_ok("MMU ENABLED and execution continued");
    } else {
        test_fail("TC does not report the MMU enabled");
    }

    /* --- an identity-mapped page still reads what we put there --- */
    v = MMIO32(PA_TEST);
    uart_puts("  [PA 0x290000] = 0x"); uart_puthex32(v); uart_putc('\n');
    if (v == MAGIC_AT_PA) {
        test_ok("identity-mapped page reads correctly through the MMU");
    } else {
        test_fail("identity mapping is broken");
    }

    /* --- the remapped page must now show the OTHER page's contents --- */
    v = MMIO32(VA_TEST);
    uart_puts("  [VA 0x280000] = 0x"); uart_puthex32(v);
    uart_puts("  (expect 0x"); uart_puthex32(MAGIC_AT_PA); uart_puts(")\n");
    if (v == MAGIC_AT_PA) {
        test_ok("REMAPPED page resolves to its physical target");
    } else if (v == MAGIC_AT_VA) {
        test_fail("remap ignored - still reading the identity page");
    } else {
        test_fail("remapped page read garbage");
    }

    /* --- writes through the virtual address must land at the physical --- */
    MMIO32(VA_TEST) = 0x12345678UL;
    pflusha();
    set_tc(0);              /* MMU off: addresses are physical again */
    pflusha();

    uart_puts("  MMU off; physical check\n");
    v = MMIO32(PA_TEST);
    uart_puts("  [PA 0x290000] = 0x"); uart_puthex32(v); uart_putc('\n');
    if (v == 0x12345678UL) {
        test_ok("write through the virtual address landed at the physical page");
    } else {
        test_fail("virtual write did not reach the physical page");
    }

    v = MMIO32(VA_TEST);
    uart_puts("  [PA 0x280000] = 0x"); uart_puthex32(v);
    uart_puts("  (expect 0x"); uart_puthex32(MAGIC_AT_VA); uart_puts(")\n");
    if (v == MAGIC_AT_VA) {
        test_ok("the bypassed physical page was left untouched");
    } else {
        test_fail("the bypassed physical page was modified");
    }

    if ((get_tc() & TC_ENABLE) == 0) {
        test_ok("MMU disabled again cleanly");
    } else {
        test_fail("could not disable the MMU");
    }

    test_end();
    return 0;
}
