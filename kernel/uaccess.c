/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * uaccess.c - the software table walk, and the copies built on it.
 */
#include "uaccess.h"
#include "task.h"
#include "vm.h"
#include "pmm.h"
#include "errno.h"
#include "string.h"

/*
 * Normally there is nothing to set: the memory a system call reaches
 * into belongs to the task that made it, and `current->as` says which
 * that is. The override exists for the one case that is not true -- the
 * loader, filling an address space that no task is running in yet.
 *
 * This used to be the only mechanism, with exec setting it around the
 * program's whole run. That worked while a program ran INSIDE the
 * spawning call and stopped working the moment a program became a task
 * of its own: nothing was setting it any more, every user pointer looked
 * like a kernel pointer, and the first write() handed the terminal an
 * address in a different address space.
 */
/* PER TASK now: current->ua_override. It was a global -- safe while
 * nothing between uaccess_set() and its undoing could sleep, and wrong
 * the moment exec could: a task that ran while exec waited for the disk
 * found its system calls reaching into another program's memory. */

/*
 * exec_spawn used to save uaccess_current() and restore it. When a
 * PROGRAM spawns, that is the program's own address space, not "no
 * override" -- so the restore installed it as a permanent override, and
 * from then on every task's system calls read and wrote the spawning
 * program's memory. The child's output was the parent's data and its
 * nanosleep read a time from the parent's stack. It healed the next
 * time the shell spawned anything, which is why nothing had noticed:
 * until signals, no program spawned another.
 */
struct addrspace *uaccess_set(struct addrspace *as)
{
    struct addrspace *was;

    if (!current) {
        return 0;
    }
    was = current->ua_override;
    current->ua_override = as;
    return was;
}

struct addrspace *uaccess_current(void)
{
    if (!current) {
        return 0;
    }
    if (current->ua_override) {
        return current->ua_override;
    }
    return current->as;
}

/*
 * How much of this page is left, capped at what was asked for.
 *
 * Every loop here works in these units, because a user range that is
 * contiguous in the program's address space is not contiguous in
 * physical memory -- consecutive virtual pages come from wherever the
 * allocator had one. Crossing a page boundary without noticing would
 * read the wrong memory and would do it silently.
 */
/*
 * The kernel's side of demand paging. A user page may be lazy, in the
 * swap file, or copy-on-write -- all of which the MMU would fault on,
 * and vm_fault() would resolve, had the PROGRAM touched it. The kernel
 * walks the tables instead of touching, so it does what the fault
 * would: asks vm_fault() and looks again.
 */
static u32 translate(struct addrspace *as, u32 uva, int write)
{
    u32 pa = vm_translate(as, uva, write);

    if (!pa && vm_fault(as, uva, write) >= 0) {
        pa = vm_translate(as, uva, write);
    }
    return pa;
}

static u32 chunk_len(u32 uva, u32 len)
{
    u32 to_end = (u32)PAGE_SIZE - (uva & PAGE_MASK);

    return len < to_end ? len : to_end;
}

void *uaccess_chunk(u32 uva, u32 *len, int write)
{
    u32 pa, n;

    struct addrspace *as = uaccess_current();

    if (!as || !len || *len == 0) {
        return 0;
    }
    pa = translate(as, uva, write);
    if (!pa) {
        return 0;
    }
    n = chunk_len(uva, *len);
    *len = n;

    /* A physical address is a kernel address: the kernel's map is
     * identity for all of RAM. */
    return (void *)pa;
}

int uaccess_check(u32 uva, u32 len, int write)
{
    struct addrspace *as = uaccess_current();

    if (!as) {
        return -EFAULT;
    }
    if (len == 0) {
        return 0;
    }
    /* Wrapping past the end of the address space would otherwise make a
     * short range look like it started high and ended low, and pass. */
    if (uva + len < uva) {
        return -EFAULT;
    }

    while (len > 0) {
        u32 n = chunk_len(uva, len);

        if (!translate(as, uva, write)) {
            return -EFAULT;
        }
        uva += n;
        len -= n;
    }
    return 0;
}

int copy_from_user(void *dst, u32 uva, u32 len)
{
    u8 *out = dst;

    if (!uaccess_current()) {
        return -EFAULT;
    }
    if (uva + len < uva) {
        return -EFAULT;
    }

    while (len > 0) {
        u32 n = len;
        void *src = uaccess_chunk(uva, &n, 0);

        if (!src) {
            return -EFAULT;
        }
        memcpy(out, src, n);
        out += n;
        uva += n;
        len -= n;
    }
    return 0;
}

int copy_to_user(u32 uva, const void *src, u32 len)
{
    const u8 *in = src;

    if (!uaccess_current()) {
        return -EFAULT;
    }
    if (uva + len < uva) {
        return -EFAULT;
    }

    while (len > 0) {
        u32 n = len;
        void *dst = uaccess_chunk(uva, &n, 1);

        if (!dst) {
            return -EFAULT;
        }
        memcpy(dst, in, n);
        in += n;
        uva += n;
        len -= n;
    }
    return 0;
}

int strncpy_from_user(char *dst, u32 uva, u32 max)
{
    u32 done = 0;

    if (!uaccess_current() || max == 0) {
        return -EFAULT;
    }

    while (done < max) {
        u32 n = max - done;
        const char *src = uaccess_chunk(uva, &n, 0);
        u32 i;

        if (!src) {
            return -EFAULT;
        }
        for (i = 0; i < n; i++) {
            dst[done] = src[i];
            if (src[i] == '\0') {
                return (int)done;
            }
            done++;
        }
        uva += n;
    }

    /*
     * Ran out of room before finding the terminator. The buffer is left
     * unterminated on purpose -- returning an error and a string would
     * invite somebody to use the string.
     */
    return -ENAMETOOLONG;
}
