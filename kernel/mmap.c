/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * mmap.c - mmap, munmap and mprotect.
 *
 * THERE IS NO TABLE OF MAPPINGS. The page tables already say, for every
 * page, whether the address space owns it and what may be done to it --
 * including a PROT_NONE page, which vm.c keeps as an invalid descriptor
 * that still holds its physical address. So munmap of part of a mapping
 * needs no splitting, mprotect of part of one needs no splitting, and
 * vm_destroy frees mapped pages the same way it frees everything else.
 * A table would be a second description of the same thing, and the day
 * the two disagreed would be a bad day.
 *
 * What that costs: there is no record of which pages came from which
 * mmap() call, so nothing like /proc/self/maps can be produced. Nothing
 * needs it yet.
 *
 * WHERE THINGS GO. Mappings are placed top down, from just below the
 * stack's guard page, and the heap grows up from the image. Both get the
 * whole gap between them, which is the layout Linux uses for the same
 * reason. brk refuses to grow over a mapped page, and placement never
 * chooses a page below the current break.
 *
 * FILE MAPPINGS ARE COPIES, read at mmap() time. There is no demand
 * paging (progress.md task 21), so there is nothing that could fault a
 * page in later. MAP_PRIVATE is therefore exact. MAP_SHARED is accepted
 * READ-ONLY and refused with PROT_WRITE: a read-only shared mapping
 * differs from the real thing only in not seeing somebody else's later
 * writes to the file, while a writable one would silently not write the
 * file at all -- and a shared mapping that does not share is the kind of
 * lie that costs somebody a day.
 */
#include "mmap.h"
#include "vm.h"
#include "pmm.h"
#include "vfs.h"
#include "task.h"
#include "textcache.h"
#include "errno.h"

/* The same allowance brk makes for the tables the pages will need. */
static int enough_memory(u32 pages)
{
    return pmm_available() >= pages + pages / 448 + 2;
}

static int range_ok(u32 addr, u32 len)
{
    return addr >= USER_VA_BASE && len <= USER_VA_END - addr;
}

static int range_free(struct addrspace *as, u32 addr, u32 len)
{
    u32 va;

    for (va = addr; va < addr + len; va += PAGE_SIZE) {
        if (vm_is_mapped(as, va)) {
            return 0;
        }
    }
    return 1;
}

/*
 * The highest free run of `len` bytes that sits above the break and
 * below the stack's guard page. Returns 0 if there is none.
 */
static u32 find_free(struct addrspace *as, u32 len)
{
    u32 floor = PAGE_ALIGN_UP(as->brk_cur);
    u32 top = USER_BRK_LIMIT;           /* first byte NOT available */
    u32 run = 0;

    while (top > floor) {
        u32 va = top - PAGE_SIZE;

        if (vm_is_mapped(as, va)) {
            run = 0;
        } else {
            run += PAGE_SIZE;
            if (run == len) {
                return va;
            }
        }
        top = va;
    }
    return 0;
}

static int prot_flags(u32 prot)
{
    if (prot == PROT_NONE) {
        return VM_NONE;
    }
    return (prot & PROT_WRITE) ? VM_WRITE : 0;
}

/* Copy the file's bytes in, leaving the descriptor's position alone. */
static int fill_from_file(struct addrspace *as, int fd, u32 addr, u32 len,
                          u32 offset)
{
    s32 saved = fd_lseek(fd, 0, SEEK_CUR);
    u32 va;
    int err = 0;

    if (saved < 0) {
        return (int)saved;
    }
    for (va = addr; va < addr + len; va += PAGE_SIZE) {
        /* Written through the kernel's identity map, before any
         * protection is applied: a read-only mapping still has to be
         * filled by somebody. */
        u32 pa = vm_translate(as, va, 0);
        s32 got;

        if (!pa || fd_lseek(fd, (s32)(offset + (va - addr)), SEEK_SET) < 0) {
            err = -EIO;
            break;
        }
        got = fd_read(fd, (void *)pa, PAGE_SIZE);
        if (got < 0) {
            err = (int)got;
            break;
        }
        /*
         * A short read is the end of the file. The rest of the page is
         * already zero, which is what POSIX requires of the tail of the
         * last page. Pages wholly past the end are zero too, where Linux
         * would raise SIGBUS on touching them; with every page read now
         * there is no touch to catch.
         */
    }
    fd_lseek(fd, saved, SEEK_SET);
    return err;
}

s32 do_mmap(u32 addr, u32 len, u32 prot, u32 flags, int fd, u32 offset)
{
    struct addrspace *as = current ? current->as : 0;
    u32 type = flags & (MAP_SHARED | MAP_PRIVATE);
    int anon = (flags & MAP_ANONYMOUS) != 0;
    int fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    u32 va, pages;
    int err;

    if (!as) {
        return -ENODEV;         /* a kernel task has no user space */
    }
    if (len == 0 || (type != MAP_SHARED && type != MAP_PRIVATE)) {
        return -EINVAL;
    }
    if (offset & PAGE_MASK) {
        return -EINVAL;
    }
    if (len > USER_VA_SIZE) {
        return -ENOMEM;
    }
    len = PAGE_ALIGN_UP(len);
    pages = len / PAGE_SIZE;

    if (!anon) {
        struct stat st;

        err = vfs_fstat(fd, &st);
        if (err < 0) {
            return err;         /* -EBADF for a descriptor not open */
        }
        if (!S_ISREG(st.st_mode)) {
            return -ENODEV;     /* a terminal, a socket: nothing to copy */
        }
        if (type == MAP_SHARED && (prot & PROT_WRITE)) {
            return -ENODEV;
        }
    }

    if (fixed) {
        if (addr & PAGE_MASK) {
            return -EINVAL;
        }
        if (!range_ok(addr, len)) {
            return -ENOMEM;
        }
        if ((flags & MAP_FIXED_NOREPLACE) && !range_free(as, addr, len)) {
            return -EEXIST;
        }
    } else {
        /* A hint is used when it is free; otherwise it is only a hint. */
        addr = PAGE_ALIGN_DOWN(addr);
        if (!addr || !range_ok(addr, len) ||
            addr < PAGE_ALIGN_UP(as->brk_cur) ||
            addr + len > USER_BRK_LIMIT || !range_free(as, addr, len)) {
            addr = find_free(as, len);
            if (!addr) {
                return -ENOMEM;
            }
        }
    }

    /*
     * An anonymous mapping takes no pages now (demand paging): what is
     * asked is whether they could be had, memory and swap together. A
     * file's pages are read in at once, so they must be there.
     */
    if (anon ? !vm_commit_ok(pages) : !enough_memory(pages)) {
        return -ENOMEM;
    }

    /* MAP_FIXED discards whatever was there, which is its definition. */
    for (va = addr; va < addr + len; va += PAGE_SIZE) {
        vm_unmap(as, va);
    }

    /*
     * A file mapped privately and READ-ONLY -- a shared library's text
     * -- is mapped from textcache.c, which hands every process the same
     * physical pages. Nothing can write to them: a later mprotect that
     * asks to is given its own copy (vm_protect). PROT_NONE is left to
     * the ordinary path; there is nothing to share in a page nobody can
     * read.
     */
    if (!anon && type == MAP_PRIVATE && !(prot & PROT_WRITE) &&
        (prot & (PROT_READ | PROT_EXEC))) {
        for (va = addr; va < addr + len; va += PAGE_SIZE) {
            u32 pa = textcache_get(fd, offset + (va - addr));

            if (!pa || !vm_map(as, va, pa, VM_USER)) {
                if (pa) {
                    pmm_free(pa);
                }
                while (va > addr) {
                    va -= PAGE_SIZE;
                    vm_unmap(as, va);
                }
                return -ENOMEM;
            }
        }
        return (s32)addr;
    }

    if (anon) {
        int flags = prot_flags(prot);

        for (va = addr; va < addr + len; va += PAGE_SIZE) {
            if (vm_map_lazy(as, va, VM_USER | flags) < 0) {
                while (va > addr) {
                    va -= PAGE_SIZE;
                    vm_unmap(as, va);
                }
                return -ENOMEM;
            }
        }
        return (s32)addr;
    }

    for (va = addr; va < addr + len; va += PAGE_SIZE) {
        if (!vm_map(as, va, 0, VM_USER | VM_WRITE)) {
            while (va > addr) {
                va -= PAGE_SIZE;
                vm_unmap(as, va);
            }
            return -ENOMEM;
        }
    }

    if (!anon) {
        err = fill_from_file(as, fd, addr, len, offset);
        if (err < 0) {
            for (va = addr; va < addr + len; va += PAGE_SIZE) {
                vm_unmap(as, va);
            }
            return err;
        }
    }

    if (prot_flags(prot) != VM_WRITE) {
        for (va = addr; va < addr + len; va += PAGE_SIZE) {
            vm_protect(as, va, prot_flags(prot));
        }
    }
    return (s32)addr;
}

int do_munmap(u32 addr, u32 len)
{
    struct addrspace *as = current ? current->as : 0;
    u32 va;

    if (!as) {
        return -ENODEV;
    }
    if ((addr & PAGE_MASK) || len == 0 || !range_ok(addr, len)) {
        return -EINVAL;
    }
    len = PAGE_ALIGN_UP(len);
    if (!range_ok(addr, len)) {
        return -EINVAL;
    }
    /* Unmapping what is not mapped is not an error, on Linux or here. */
    for (va = addr; va < addr + len; va += PAGE_SIZE) {
        vm_unmap(as, va);
    }
    return 0;
}

int do_mprotect(u32 addr, u32 len, u32 prot)
{
    struct addrspace *as = current ? current->as : 0;
    u32 va;

    if (!as) {
        return -ENODEV;
    }
    if ((addr & PAGE_MASK) || !range_ok(addr, len)) {
        return -EINVAL;
    }
    len = PAGE_ALIGN_UP(len);
    if (!range_ok(addr, len)) {
        return -EINVAL;
    }
    /* All or nothing: every page must exist before any is changed. */
    for (va = addr; va < addr + len; va += PAGE_SIZE) {
        if (!vm_is_mapped(as, va)) {
            return -ENOMEM;
        }
    }
    for (va = addr; va < addr + len; va += PAGE_SIZE) {
        if (vm_protect(as, va, prot_flags(prot)) < 0) {
            return -ENOMEM;     /* no page to copy a shared one into */
        }
    }
    return 0;
}
