/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * sage040-crt1.c - where a musl program starts on this machine.
 *
 * musl's own crt1 expects Linux's initial stack: argc at the stack
 * pointer, then argv[], a NULL, envp[], a NULL and the auxiliary
 * vector, one after another -- it finds envp at argv + argc + 1. This
 * kernel's stack holds a dummy return address, argc, and POINTERS to
 * argv and envp (kernel/exec.c, setup_stack), which is what this
 * system's crt0 reads. The auxiliary vector does follow envp's NULL, as
 * on Linux.
 *
 * So the block musl wants is built here, from what is there, on this
 * function's own stack -- which is never returned through:
 * __libc_start_main ends in exit() -- and handed to musl.
 */
int main(int, char **, char **);
int __libc_start_main(int (*)(int, char **, char **), int, char **,
                      void (*)(void), void (*)(void), void (*)(void));
void _init(void);
void _fini(void);

__attribute__((noreturn, used)) void __sage040_musl_start(long *sp)
{
    int argc = (int)sp[1];
    char **argv = (char **)sp[2];
    char **envp = (char **)sp[3];
    long *aux;
    int envc = 0, auxw = 0, i, n = 0;

    while (envp[envc]) {
        envc++;
    }
    aux = (long *)(envp + envc + 1);
    while (aux[auxw]) {
        auxw += 2;
    }
    auxw += 2;                          /* and AT_NULL's pair */
    {
        long block[1 + argc + 1 + envc + 1 + auxw];

        block[n++] = argc;
        for (i = 0; i < argc; i++) {
            block[n++] = (long)argv[i];
        }
        block[n++] = 0;
        for (i = 0; i < envc; i++) {
            block[n++] = (long)envp[i];
        }
        block[n++] = 0;
        for (i = 0; i < auxw; i++) {
            block[n++] = aux[i];
        }
        __libc_start_main(main, argc, (char **)(block + 1), _init, _fini, 0);
    }
    for (;;) {
    }
}

__asm__(
    ".text\n"
    ".global _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "   move.l %sp,-(%sp)\n"
    "   jsr __sage040_musl_start\n"
);
