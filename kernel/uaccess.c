/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * uaccess.c - the software table walk, and the copies built on it.
 */
#include "uaccess.h"
#include "vm.h"
#include "pmm.h"
#include "errno.h"
#include "string.h"

static struct addrspace *current;

void uaccess_set(struct addrspace *as)
{
    current = as;
}

struct addrspace *uaccess_current(void)
{
    return current;
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
static u32 chunk_len(u32 uva, u32 len)
{
    u32 to_end = (u32)PAGE_SIZE - (uva & PAGE_MASK);

    return len < to_end ? len : to_end;
}

void *uaccess_chunk(u32 uva, u32 *len, int write)
{
    u32 pa, n;

    if (!current || !len || *len == 0) {
        return 0;
    }
    pa = vm_translate(current, uva, write);
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
    if (!current) {
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

        if (!vm_translate(current, uva, write)) {
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

    if (!current) {
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

    if (!current) {
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

    if (!current || max == 0) {
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
