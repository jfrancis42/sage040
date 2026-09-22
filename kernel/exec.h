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
#define EXEC_MAX_ARGS   256
#define EXEC_MAX_ENV    256

/*
 * Load `path` and start it as a task. Returns the new pid, or -errno.
 *
 * This is still not execve(). execve replaces the caller; this makes a
 * new task and leaves the caller running, which is fork and execve in
 * one step. Whether to wait for it is the caller's decision, and that
 * decision is what `&` is.
 */
int exec_spawn(const char *path, int argc, char **argv, char **envp);

/*
 * execve(): replace the CURRENT task's program with `path`, rewriting
 * `regs` so that the return to user mode enters it. The new image is
 * built completely before the old one is touched, so a failure leaves
 * the caller exactly as it was, with the error to report. Returns 0 --
 * into the new program -- or -errno into the old one.
 */
struct pt_regs;
int exec_replace(const char *path, int argc, char **argv, char **envp,
                 struct pt_regs *regs);

#endif /* EXEC_H */
