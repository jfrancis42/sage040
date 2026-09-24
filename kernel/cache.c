/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * cache.c - the 68040's caches, and the system call that flushes them.
 *
 * THE PROBLEM THIS EXISTS FOR. The 68040 has separate data and
 * instruction caches. A program that writes instructions into memory
 * and jumps to them -- libffi's trampolines, a JIT, a signal return
 * sequence built on the stack -- has stored bytes that may still be
 * sitting in the DATA cache, while the INSTRUCTION cache holds whatever
 * used to be at that address. The store is invisible to the fetch. On
 * a machine with one cache, or none, the code just works; on this one
 * it works until the day it does not, and the failure is a jump into
 * stale instructions.
 *
 * Only the supervisor can do anything about it -- `cpusha` and `cinva`
 * are privileged -- so a program has to ask, and Linux/m68k's answer is
 * the cacheflush(2) system call. It is one of the very few calls that
 * exists on m68k and nowhere else, and this is it.
 *
 * WHAT IT DOES TODAY. The caches are OFF: nothing writes CACR, so both
 * are disabled and there is genuinely nothing to push or invalidate.
 * The instructions are still issued, because they are correct, cost
 * four cycles, and mean that whoever turns the caches on does not have
 * to come back here. Under QEMU they decode as privileged no-ops
 * (target/m68k/translate.c: "Cache push/invalidate. Implement as
 * no-op."), so this is exercised but cannot be OBSERVED to work by
 * running it -- on emulated hardware with no caches, a correct flush
 * and a missing one look identical. That is worth knowing before
 * trusting a passing test here to mean anything about real silicon.
 *
 * The whole cache is pushed whatever scope was asked for. A line flush
 * would be `cpushl bc,(a0)` per line; flushing everything is a superset
 * of every request, it is what Linux/m68k does for anything bigger than
 * a couple of pages, and with the caches off it makes no difference at
 * all. The address range is still checked, because a program passing a
 * bad pointer should hear about it now rather than when caches are
 * switched on.
 */

#include "kernel.h"
#include "uapi.h"
#include "errno.h"
#include "uaccess.h"

/*
 * Push every dirty line out of the data cache and invalidate both --
 * `cpusha bc`, the 68040's "push and invalidate all, both caches".
 * Privileged; this only ever runs in the kernel.
 */
void cache_flush_all(void)
{
    __asm__ __volatile__("cpusha %%bc" ::: "memory");
}

/*
 * Turn both caches on.
 *
 * CACR on the 68040 has exactly two bits that mean anything: bit 31
 * enables the data cache and bit 15 the instruction cache. Everything
 * else is reserved and reads back zero -- QEMU masks a write with
 * 0x80008000 for the 040, which is a second source for the same two
 * bits.
 *
 * INVALIDATE FIRST. The caches come out of reset with undefined tags,
 * so enabling them without `cinva` lets the processor answer a read
 * from a line that was never loaded. cpusha is not enough on its own:
 * pushing writes dirty lines OUT, and what is needed here is throwing
 * whatever is in them AWAY.
 *
 * WHAT HAS TO BE TRUE BEFORE THIS IS CALLED, because none of it can be
 * checked here and none of it can be observed on the emulator:
 *
 *   - the MMU is on, so the CM bits in the page descriptors are what
 *     decide cachability per page (before that, everything would be
 *     cached according to the transparent translation registers alone);
 *   - device registers are NON-CACHABLE -- DTT0 covers the I/O window
 *     and DTT1 the framebuffer, both CM_NC, or the first read of a UART
 *     status register would be answered from the cache for ever;
 *   - every page holding TRANSLATION TABLES is non-cachable, because
 *     the MMU's table walker neither reads through the data cache nor
 *     snoops it (vm.c, kset_cachemode).
 *
 * THIS CANNOT BE TESTED HERE. QEMU has no cache model: it accepts the
 * CACR write, ignores the CM bits entirely, and decodes cinv and cpush
 * as no-ops. A correct cache setup and a broken one are identical
 * under the emulator, and there is no speedup to measure either. It is
 * written for real hardware and it is correct by inspection -- which
 * is the reason for the list above rather than a comment saying it
 * works.
 */
#define CACR_DATA_ENABLE    0x80000000UL
#define CACR_INSTR_ENABLE   0x00008000UL

void cache_enable(void)
{
    u32 cacr = CACR_DATA_ENABLE | CACR_INSTR_ENABLE;

    /* Throw away whatever the tags happen to hold, both caches. */
    __asm__ __volatile__("cinva %%bc" ::: "memory");
    __asm__ __volatile__("movec %0,%%cacr" :: "d"(cacr) : "memory");
}

u32 cache_state(void)
{
    u32 cacr;

    __asm__ __volatile__("movec %%cacr,%0" : "=d"(cacr));
    return cacr;
}

/*
 * cacheflush(addr, scope, cache, len), Linux/m68k's call.
 *
 * Linux restricts FLUSH_SCOPE_ALL to root because a process could
 * otherwise slow the machine down for everyone; there is one user here
 * and no privilege to check against, so every scope is allowed.
 */
int do_cacheflush(u32 addr, int scope, int cache, u32 len)
{
    switch (scope) {
    case FLUSH_SCOPE_LINE:
    case FLUSH_SCOPE_PAGE:
        /*
         * A range the caller cannot reach is EINVAL, which is what
         * Linux answers: the argument is wrong, rather than a fault
         * having been taken on it.
         */
        if (len && uaccess_check(addr, len, 0) < 0) {
            return -EINVAL;
        }
        break;
    case FLUSH_SCOPE_ALL:
        break;
    default:
        return -EINVAL;
    }

    switch (cache) {
    case FLUSH_CACHE_DATA:
    case FLUSH_CACHE_INSN:
    case FLUSH_CACHE_BOTH:
        break;
    default:
        return -EINVAL;
    }

    cache_flush_all();
    return 0;
}
