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
#include "vm.h"

/*
 * Where a program lives: in an address space of its own.
 *
 * The layout is in vm.h, because it is a property of the memory system
 * rather than of the loader. What matters here is that the numbers are
 * VIRTUAL and every program has the same ones -- two programs both
 * begin at USER_VA_BASE, on different physical pages, and neither can
 * see the other or the kernel.
 *
 * Until recently these were physical addresses in the kernel's own
 * space, and the gap below the stack was a convention rather than a
 * rule. It is now a hole in a page table.
 */
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

/* The address space of the running program, or null. */
struct addrspace *exec_addrspace(void);

#endif /* EXEC_H */
