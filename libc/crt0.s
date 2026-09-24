| SPDX-License-Identifier: GPL-3.0-or-later
| Copyright (C) 2026 Jeff Francis
|
| crt0.s - where a program linked with picolibc starts.
|
| The kernel enters exactly as it does for lib/crt0.s -- by jsr, so
| above the return address:
|
|       4(%sp)  argc
|       8(%sp)  argv
|      12(%sp)  envp
|
| and .bss is already clear, because the loader clears it. What this
| adds for a real C library is its environ, and its constructors:
| __libc_init_array runs .preinit_array and .init_array, which is where
| a static initialiser with side effects ends up, and exit() runs the
| destructors and flushes stdio on the way out.
|
| No TLS to set up: picolibc is built with thread-local storage off
| (libc/build.sh), because a process here has one thread.

        .section .text.start
        .globl  _start
        .type   _start,@function
_start:
        move.l  12(%sp),environ         | picolibc's own, from stdlib
        move.l  8(%sp),%a0              | argv[0], for program_invocation_name
        move.l  (%a0),-(%sp)            |   and getprogname (posix-more.c)
        jsr     __sage040_progname
        addq.l  #4,%sp
        jsr     __libc_init_array
        move.l  12(%sp),-(%sp)          | envp
        move.l  12(%sp),-(%sp)          | argv (the stack moved by 4)
        move.l  12(%sp),-(%sp)          | argc (and by 4 again)
        jsr     main
        lea     12(%sp),%sp
        move.l  %d0,-(%sp)
        jsr     exit                    | does not return
1:      bra.s   1b
        .size   _start, . - _start

| --- __dso_handle -----------------------------------------------------
|
| WHAT THIS IS. A C++ program with a static object registers its
| destructor with __cxa_atexit(fn, obj, &__dso_handle), and the third
| argument names WHICH shared object the destructor belongs to, so that
| unloading one runs only its own. Every executable is supposed to
| define its own; on Linux that definition comes from crtbegin.o, which
| gcc's driver links in. This toolchain's specs link crt0 and nothing
| else -- picolibc's crt0 already walks the init and fini arrays, which
| is the other half of what crtbegin does -- so nothing provided one.
|
| WHY IT MATTERS, AND WHY IT ONLY BIT ON ONE MACHINE. gcc marks the
| reference HIDDEN when its configure finds an assembler that supports
| hidden visibility, and a hidden reference may not be satisfied by a
| shared library -- that is what hidden means. On a machine whose gcc
| had decided the assembler could not, the reference was DEFAULT
| visibility and quietly bound to libc.so's copy through a copy
| relocation, and the native compiler built. On a machine whose gcc
| decided it could, the same source stopped with
|
|     ld: Tcollect2: hidden symbol `__dso_handle' isn't defined
|
| in the middle of building gcc, three steps from anything to do with
| this. The second machine was RIGHT; the first was getting away with
| using libc's handle to identify the program, which is the wrong
| object.
|
| Defined here rather than by dragging in crtbegin.o: crt0 is already
| linked into every program, and crtbegin brings constructor machinery
| that picolibc's crt0 does itself. Hidden, so it is this program's own
| and never exported; pointing at itself, which is what glibc's
| crtbegin does for an executable -- the VALUE is never dereferenced,
| only its address is used, so what matters is that it is unique to the
| program.
        .data
        .globl  __dso_handle
        .hidden __dso_handle
        .p2align 2
        .type   __dso_handle,@object
        .size   __dso_handle, 4
__dso_handle:
        .long   __dso_handle
