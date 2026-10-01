#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# cxx.sh - g++ for this machine, as the C++ ports invoke it: the second
# cross compiler (install-cxx), picolibc's headers, libstdc++'s, and
# libc/sage040.specs for the link. Arguments are passed through; add
# -static for a static program and -lstdc++ to link the library.

set -eu
. "$(dirname "$0")/../../ports/cross.sh"
trap - EXIT
CXXPREFIX=${SAGE_CXX:-$HOME/m68k/install-cxx}
CXXLIB=$SRCDIR/build-libstdcxx-sage040/sage040
LIBGCC=$(dirname "$("$CROSS_CC" -mcpu=68040 -print-libgcc-file-name)")
exec "$CXXPREFIX/bin/m68k-elf-g++" \
    -B"$(dirname "$CROSS_CC")/../m68k-elf/bin/" -B"$LIBGCC/" -L"$LIBGCC" -L"$CXXLIB/lib" \
    -mcpu=68040 -O2 -nostdinc -isystem "$SAGE_LIBC/include" \
    -isystem "$("$CXXPREFIX/bin/m68k-elf-gcc" -print-file-name=include)" -D_GNU_SOURCE \
    -I"$CXXLIB/include/c++/15.2.0" -I"$CXXLIB/include/c++/15.2.0/m68k-unknown-elf" \
    $SPECS_CFLAGS "$@"
