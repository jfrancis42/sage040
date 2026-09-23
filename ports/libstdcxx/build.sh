#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - libstdc++ for SuckOS: the C++ standard library.
#
# WHY THIS EXISTS AT ALL. GCC 15 is written in C++, and so is GDB. A
# compiler that RUNS ON this machine therefore needs a C++ runtime that
# runs on this machine -- which means libstdc++ has to be built for the
# target before the native gcc can be built at all. It is the middle
# link of the chain, and the reason the native toolchain is three
# builds rather than one.
#
# It is also worth having on its own: a system that can only build C is
# a system that half the world cannot be ported to.
#
# BUILT WITH THE CROSS g++ FROM ITS OWN PREFIX. ~/m68k/install is the
# C-only cross toolchain everything else in this tree depends on, and
# it stays untouched; ~/m68k/install-cxx is the same gcc 15.2.0 built
# with c,c++ as well. Same version, same target, same ABI -- so an
# object from one links against an object from the other -- and the
# working compiler is never at risk from this.
#
# --with-newlib is the important one: picolibc is newlib's descendant,
# and that option is how libstdc++ is told not to assume glibc's
# extensions exist. Without it the build asks for things no freestanding
# C library has.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=15.2.0
SRC=$SRCDIR/gcc-$VERSION
BUILD=$SRCDIR/build-libstdcxx-sage040
CXXPREFIX=${SAGE_CXX:-$HOME/m68k/install-cxx}

[ -x "$CXXPREFIX/bin/m68k-elf-g++" ] || {
    echo "libstdc++: no cross g++ in $CXXPREFIX." >&2
    echo "It is gcc $VERSION configured --enable-languages=c,c++"  >&2
    echo "into a prefix of its own; see progress.md, task 49."     >&2
    exit 1
}

CXXCC=$CXXPREFIX/bin/m68k-elf-gcc
CXXCXX=$CXXPREFIX/bin/m68k-elf-g++

# THE COMPILER'S OWN HEADERS MUST BE ITS OWN. cross.sh builds
# CROSS_CPPFLAGS around the C-only toolchain in ~/m68k/install, so it
# names that gcc's internal include directory -- stddef.h, stdint.h,
# stdarg.h, the ones the compiler ships rather than the C library. Used
# with a DIFFERENT gcc they are the wrong copy, and the failure is
# "cannot compute suffix of object files", which says nothing at all.
CXX_CPPFLAGS="-nostdinc -isystem $SAGE_LIBC/include \
-isystem $("$CXXCC" -print-file-name=include) -D_GNU_SOURCE"

# AND ITS ASSEMBLER AND LINKER HAVE TO BE m68k's. install-cxx holds a
# compiler and nothing else -- only `make install-gcc` was run into it,
# because binutils is already built and installing a second copy would
# be two copies to keep in step. So this gcc looks for `as` on PATH and
# finds the HOST's, which says "unrecognized option '-mcpu=68040'" and
# configure reports "cannot compute suffix of object files".
CXX_BINDIR=$(dirname "$CROSS_CC")/../m68k-elf/bin
#
# AND ITS libgcc. install-cxx was built with `make install-gcc` alone,
# which installs the compiler and not the target library -- so -lgcc
# was not there, every link test failed, and configure concluded the
# compiler could not produce executables at all
# ("Link tests are not allowed after GCC_NO_EXECUTABLES", three
# hundred lines after the actual failure).
#
# The C toolchain's libgcc is the right one to use rather than
# building a second: same gcc version, same target, same ABI, and
# libgcc's contents do not depend on which front ends were enabled.
CXX_LIBGCC=$(dirname "$("$CROSS_CC" -mcpu=68040 -print-libgcc-file-name)")
CXX_TOOLS="-B$CXX_BINDIR/ -B$CXX_LIBGCC/ -L$CXX_LIBGCC"

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/libstdc++-v3/configure" \
        --host=m68k-unknown-elf \
        --build="$("$SRC/config.guess")" \
        --prefix=/usr \
        --with-newlib \
        --disable-shared --enable-static \
        --disable-libstdcxx-pch \
        --disable-nls \
        --disable-libstdcxx-verbose \
        --enable-cstdint \
        --enable-libstdcxx-threads \
        --enable-libstdcxx-time=rt \
        --disable-sjlj-exceptions \
        --enable-tls=no \
        CC="$CXXCC $CXX_TOOLS $CROSS_CFLAGS $CXX_CPPFLAGS $SPECS_CFLAGS" \
        CXX="$CXXCXX $CXX_TOOLS $CROSS_CFLAGS $CXX_CPPFLAGS $SPECS_CFLAGS" \
        CC_FOR_BUILD=cc \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -40 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT" "$BUILD/inst"
make -C "$BUILD" install DESTDIR="$BUILD/inst" > "$BUILD/install.log" 2>&1 \
    || { tail -30 "$BUILD/install.log"; exit 1; }
mkdir -p "$OUT"
cp -a "$BUILD/inst/usr/lib" "$BUILD/inst/usr/include" "$OUT/" 2>/dev/null || true
rm -f "$OUT/lib"/*.la

echo "libstdc++ $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" -t "$OUT/lib/libstdc++.a" 2>/dev/null | tail -1
