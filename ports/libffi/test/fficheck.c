/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Jeff Francis */
/*
 * fficheck.c - libffi on this machine: does it actually call anything?
 *
 * Building libffi proves that its m68k backend compiles. It does not
 * prove that the calling convention it generates is THIS compiler's,
 * and the two are separate questions: ffi_call builds an argument
 * frame by hand, from a description, and if it disagrees with gcc
 * about where a `char` or a `double` goes then every call returns
 * plausible rubbish rather than failing.
 *
 * So each check calls a function whose arguments are chosen so that a
 * frame laid out wrongly cannot give the right answer by luck: the
 * values differ from each other, the types differ in width, and the
 * expected result depends on all of them.
 *
 * THE CLOSURE IS THE INTERESTING ONE. A closure is a small piece of
 * CODE libffi writes into memory at run time, so that a C function
 * pointer can be handed to something that knows nothing about libffi.
 * It needs memory that is both writable and executable, and on a
 * 68040 it needs the instruction cache to be told -- which is why
 * this system has cacheflush(2) at all. If mmap here will not give
 * out executable pages, this is where it shows.
 */

#include <ffi.h>
#include <stdio.h>
#include <string.h>

static int pass, fail;

static void
check(const char *what, int ok)
{
    if (ok) {
        pass++;
        printf("  [ OK ] %s\n", what);
    } else {
        fail++;
        printf("  [FAIL] %s\n", what);
    }
    fflush(stdout);
}

/* Mixed widths on purpose: a frame built wrongly cannot produce this. */
static long
mixer(char a, short b, int c, long d)
{
    return (long)a * 1000000 + (long)b * 1000 + (long)c * 10 + d;
}

static double
adder(double a, float b, double c)
{
    return a + (double)b + c;
}

static const char *
picker(const char *a, const char *b, int which)
{
    return which ? b : a;
}

/* What the closure will do when something calls it as a plain C
 * function pointer. */
static void
closure_body(ffi_cif *cif, void *ret, void **args, void *user)
{
    int a = *(int *)args[0];
    int b = *(int *)args[1];

    (void)cif;
    *(ffi_arg *)ret = (ffi_arg)(a * 100 + b + *(int *)user);
}

typedef int (*adder_fn)(int, int);

int
main(void)
{
    ffi_cif cif;
    ffi_type *args[4];
    void *vals[4];

    printf("fficheck: libffi on SuckOS\n");

    /* --- integers of several widths --------------------------------- */
    {
        char a = 7;
        short b = 123;
        int c = 45;
        long d = 6;
        ffi_arg result = 0;
        long want = 7L * 1000000 + 123L * 1000 + 45L * 10 + 6L;

        args[0] = &ffi_type_schar;
        args[1] = &ffi_type_sshort;
        args[2] = &ffi_type_sint;
        args[3] = &ffi_type_slong;
        vals[0] = &a; vals[1] = &b; vals[2] = &c; vals[3] = &d;

        check("ffi_prep_cif for (char, short, int, long) -> long",
              ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 4, &ffi_type_slong,
                           args) == FFI_OK);
        ffi_call(&cif, FFI_FN(mixer), &result, vals);
        printf("         got %ld, want %ld\n", (long)result, want);
        check("  ffi_call agrees with a direct call", (long)result == want);
        check("  and a direct call agrees with itself",
              mixer(a, b, c, d) == want);
    }

    /* --- floating point, which has its own registers and rules ------- */
    {
        double a = 1.5, c = 0.25;
        float b = 2.0f;
        double result = 0;

        args[0] = &ffi_type_double;
        args[1] = &ffi_type_float;
        args[2] = &ffi_type_double;
        vals[0] = &a; vals[1] = &b; vals[2] = &c;

        check("ffi_prep_cif for (double, float, double) -> double",
              ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_double,
                           args) == FFI_OK);
        ffi_call(&cif, FFI_FN(adder), &result, vals);
        printf("         got %f, want %f\n", result, 3.75);
        check("  ffi_call gets the floating-point frame right",
              result > 3.7499 && result < 3.7501);
    }

    /* --- pointers in and a pointer out ------------------------------ */
    {
        const char *one = "first", *two = "second";
        int which = 1;
        const char *result = 0;

        args[0] = &ffi_type_pointer;
        args[1] = &ffi_type_pointer;
        args[2] = &ffi_type_sint;
        vals[0] = &one; vals[1] = &two; vals[2] = &which;

        check("ffi_prep_cif for a function returning a pointer",
              ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_pointer,
                           args) == FFI_OK);
        ffi_call(&cif, FFI_FN(picker), &result, vals);
        check("  and the pointer that comes back is the right one",
              result != 0 && strcmp(result, "second") == 0);
    }

    /* --- a closure: code written at run time and then called --------- */
    {
        ffi_cif ccif;
        ffi_type *cargs[2];
        ffi_closure *closure;
        void *code = 0;
        int bias = 5;

        cargs[0] = &ffi_type_sint;
        cargs[1] = &ffi_type_sint;

        closure = ffi_closure_alloc(sizeof(ffi_closure), &code);
        check("ffi_closure_alloc gives writable+executable memory",
              closure != 0 && code != 0);
        if (closure && code) {
            check("  ffi_prep_cif for the closure's signature",
                  ffi_prep_cif(&ccif, FFI_DEFAULT_ABI, 2, &ffi_type_sint,
                               cargs) == FFI_OK);
            check("  ffi_prep_closure_loc writes the trampoline",
                  ffi_prep_closure_loc(closure, &ccif, closure_body,
                                       &bias, code) == FFI_OK);
            {
                /*
                 * Called as an ordinary function pointer, which is the
                 * whole point: nothing at this call site knows libffi
                 * exists.
                 */
                adder_fn f = (adder_fn)code;
                int got = f(3, 4);

                printf("         closure(3,4) = %d, want %d\n", got,
                       3 * 100 + 4 + 5);
                check("  and calling it as a plain C function works",
                      got == 3 * 100 + 4 + 5);
            }
            ffi_closure_free(closure);
        } else {
            check("  ffi_prep_cif for the closure's signature", 0);
            check("  ffi_prep_closure_loc writes the trampoline", 0);
            check("  and calling it as a plain C function works", 0);
        }
    }

    printf("\n  passed: %d\n  failed: %d\n", pass, fail);
    printf("FFI-RESULT: %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
