/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * exec.h - loading and running a program from the filesystem.
 *
 * Programs are ELF32 executables, big-endian, EM_68K -- exactly what the
 * toolchain already emits, so there is no flattening step and no private
 * format to document. The kernel decides a file is a program by looking
 * at its first four bytes, not at its name, which is what Unix has
 * always done and is the only thing that can work here: a FAT16 volume
 * has no execute permission bit to consult.
 *
 * That is also why programs carry no extension. `CUBE`, not `CUBE.EXE`.
 * The name says what the thing is; the magic number says what format it
 * is in.
 */
#ifndef EXEC_H
#define EXEC_H

#include "kernel.h"

/*
 * Where a program lives.
 *
 * The kernel occupies low memory and keeps its stack at the top of RAM.
 * A program is given the megabyte at 1 MB for its image and a stack
 * growing down from 3 MB, which leaves a megabyte of unmapped gap
 * between the two stacks. With no MMU there is nothing enforcing any of
 * this -- the gap is there so that a runaway program stack runs into
 * empty space rather than straight into the kernel's.
 *
 *   0x00000000  kernel: vectors, text, data, bss
 *   0x00100000  program image          <- USER_BASE
 *   0x002ffff0  program stack, growing down
 *   0x00300000  unused gap
 *   0x003ffff0  kernel supervisor stack
 */
#define USER_BASE       0x00100000UL
#define USER_LIMIT      0x002f0000UL    /* image must end below here   */
#define USER_STACK_TOP  0x002ffff0UL

#define EXEC_MAX_ARGS   8

/*
 * Load `path` and run it. Returns the program's exit status, or a
 * negated errno if it could not be loaded.
 *
 * This is not execve(). It does not replace the caller, because there is
 * no process to replace: it loads, calls, and comes back. When there are
 * processes, this becomes fork + execve + waitpid, the caller keeps the
 * same shape, and the syscall underneath it changes.
 */
int exec_spawn(const char *path, int argc, char **argv);

/* Called by the exit() system call. Does not return if a program is
 * running; returns -ENOSYS if one is not, because the shell has nowhere
 * to exit to. */
int exec_exit(int status);

int exec_running(void);

#endif /* EXEC_H */
