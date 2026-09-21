/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * uaccess.h - reaching into a program's memory.
 *
 * A program's pointers are addresses in ITS address space. The kernel
 * runs in a different one, where those numbers mean nothing at all --
 * 0x10001234 is a valid pointer to the program and an unmapped address
 * to the supervisor. So a system call cannot dereference what it was
 * handed. It has to ask.
 *
 * THIS IS THE POINT, NOT AN OBSTACLE. The kernel deliberately does not
 * map the user area into its own space, and the reason is what happens
 * when somebody forgets to use these functions: the raw dereference
 * faults immediately, on the first call, in the first test that touches
 * that system call. Had the user area been visible to the kernel the
 * same omission would work perfectly until a program passed a bad
 * pointer, and then it would take the machine down instead of returning
 * an error. One of those is found in an afternoon and the other in six
 * months.
 *
 * Every function here walks the program's page tables in software and
 * returns -EFAULT rather than faulting. A program is allowed to pass
 * rubbish; it gets an error back, and the kernel stays up. That is the
 * contract, and it is the whole difference between a machine with
 * memory protection and a machine with page tables.
 *
 * No fixups, no exception table. The Linux approach -- dereference and
 * recover from the fault -- is faster and needs a table of resumable
 * instructions and a fault handler that can consult it. Walking the
 * tables costs a few loads per page and needs neither, and the walk is
 * the validation rather than something done in addition to it.
 */
#ifndef UACCESS_H
#define UACCESS_H

#include "kernel.h"

struct addrspace;

/*
 * Whose memory the calls below reach into.
 *
 * Set when a program is entered and cleared when it leaves. With it
 * clear every call here fails, which is deliberate: the shell runs in
 * the kernel's own space and its pointers are already kernel pointers,
 * so a uaccess call made on its behalf is a bug rather than a special
 * case to accommodate.
 */
void uaccess_set(struct addrspace *as);
struct addrspace *uaccess_current(void);

/* 0, or -EFAULT if any part of the range is not mapped, or is not
 * writable and needed to be. */
int uaccess_check(u32 uva, u32 len, int write);

int copy_from_user(void *dst, u32 uva, u32 len);
int copy_to_user(u32 uva, const void *src, u32 len);

/*
 * A NUL-terminated string out of user memory.
 *
 * Returns its length, -EFAULT if it runs off a mapped page before the
 * NUL, or -ENAMETOOLONG if it does not end within `max`. A program that
 * passes an unterminated string gets an error, not a kernel that reads
 * until it hits something.
 */
int strncpy_from_user(char *dst, u32 uva, u32 max);

/*
 * A kernel pointer to user memory, good for up to *len bytes.
 *
 * For bulk transfers -- read() and write() -- where copying through a
 * bounce buffer would mean a second copy of every byte for no reason.
 * The caller passes the length it wants and gets back the length that is
 * contiguous in physical memory, which is never more than to the end of
 * the page. Loop until done.
 *
 * Returns null and leaves *len alone if the address is not mapped.
 */
void *uaccess_chunk(u32 uva, u32 *len, int write);

#endif /* UACCESS_H */
