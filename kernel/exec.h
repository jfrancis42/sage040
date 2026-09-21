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

/*
 * End the running program from outside it: ctrl-C.
 *
 * Safe to call from an interrupt handler, which exec_exit() is not --
 * the unwind behind this one puts the interrupt mask back, because
 * nothing is going to execute an RTE and do it. Never returns.
 */
void exec_kill(int status) __attribute__((noreturn));

/*
 * Resume a stopped job. Returns its exit status, or SPAWN_STOPPED if it
 * stopped again, or -errno.
 */
int exec_continue(int id);

/*
 * The context switch, in execasm.s.
 *
 * exec_stop() saves where the program is and returns to whoever is
 * waiting for it; exec_resume() does the reverse. Between them they are
 * the primitive a scheduler is built from, and they are here rather than
 * in a scheduler because ctrl-Z needed them first.
 */
void exec_stop(u32 *saved_sp);
int  exec_resume(u32 saved_sp);

int exec_running(void);

#endif /* EXEC_H */
