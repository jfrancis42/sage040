/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sh - the shell, as a program.
 *
 * The same kernel/shell.c and kernel/edit.c the machine's own shell is
 * built from. That works because both of them reach the system only
 * through the system call gate -- kernel/layercheck.sh enforces it on
 * every build -- so they do not care whether they run as a kernel task
 * or in user mode. This file is only the way in.
 *
 * It exists so that programs can run commands: an editor's `:!` and
 * `:make`, and system() and popen() in a C library, all run
 * `$SHELL -c COMMAND`, and there was no shell a program could exec.
 */
#include "syscall.h"

int shell_main(int argc, char **argv, char **envp);

int main(int argc, char **argv, char **envp)
{
    return shell_main(argc, argv, envp);
}

/* crt0.s calls exit() with main's return value; there is no ulib here. */
void exit(int status)
{
    sys_exit(status);
    for (;;) {
    }
}
