// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis
//
// ehtest.cc - a C++ exception, thrown three frames down and caught.
//
// It needs the program's unwind tables registered at start
// (libc/crtbegin-eh.s); without that every throw ends in libgcc's own
// assertion. Prints one line per check and exits 0 only if all pass.
// The destructor count is the part a half-working unwinder gets wrong:
// it has to run each frame's cleanups on the way up, not just land in
// the catch.
#include <cstdio>
#include <stdexcept>
#include <string>

static int destroyed;

struct Guard {
    ~Guard() { destroyed++; }
};

static int deep(int n)
{
    Guard g;
    if (n == 0)
        throw std::runtime_error("from the bottom");
    return deep(n - 1) + 1;
}

int main()
{
    int ok = 0, fail = 0;
    std::string what;

    try {
        deep(3);
    } catch (const std::exception &e) {
        what = e.what();
    }
    if (what == "from the bottom") { std::puts("ok   caught, with its message"); ok++; }
    else { std::puts("FAIL caught, with its message"); fail++; }
    if (destroyed == 4) { std::puts("ok   four frames' destructors ran on the way up"); ok++; }
    else { std::printf("FAIL four frames' destructors ran (%d)\n", destroyed); fail++; }
    try {
        throw 42;
    } catch (int v) {
        if (v == 42) { std::puts("ok   an int, caught by type"); ok++; }
        else { std::puts("FAIL an int, caught by type"); fail++; }
    }
    std::printf("EHTEST %d ok %d failed\n", ok, fail);
    return fail != 0;
}
