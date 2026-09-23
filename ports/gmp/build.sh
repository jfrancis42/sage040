#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GMP for SuckOS: arbitrary-precision arithmetic.
#
# Two things want it. gcc needs it to fold constant expressions
# exactly -- it computes in GMP/MPFR rather than in the host's
# doubles, which is why a cross compiler gives the same answers as a
# native one. And CLISP's bignums are GMP.
#
# It is built as its own port rather than left in gcc's tree because
# gcc's in-tree build would configure it with whatever the top level
# passes, and this one needs a specific answer about assembly.
#
# --disable-assembly IS THE POINT. GMP ships hand-written m68k
# assembly, written for the 68020 and still correct for the 68040,
# but it is selected by a host-triplet table that has no entry this
# build matches, and GMP's configure runs test programs to decide
# which paths are safe -- which it cannot do when the programs would
# have to run on the emulator. The generic C is what every unsupported
# host gets, and arithmetic is arithmetic: the answers are the same,
# and nothing here is bignum-bound.
#
# -std=gnu17 BECAUSE GCC 15 DEFAULTS TO C23, in which `void g(){}` is
# a function taking NO arguments rather than one taking an unspecified
# number. GMP 6.2.1 is from 2020 and one of its own configure probes
# declares `void g(){}` and then calls it with six arguments -- so the
# probe failed to compile, and GMP concluded "could not find a working
# compiler". The library itself is fine; it is the 2020 dialect that
# has to be asked for.
#
# The version is the one gcc 15.2.0 asks for (gcc/contrib/
# download_prerequisites), so gcc and CLISP share one library rather
# than disagreeing about it.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=6.2.1
SRC=$SRCDIR/gmp-$VERSION
BUILD=$SRCDIR/build-gmp-sage040

[ -d "$SRC" ] || {
    echo "gmp: $SRC is not there -- gcc's download_prerequisites puts it" >&2
    echo "in the gcc source tree; this expects it unpacked in $SRCDIR."   >&2
    exit 1
}

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --build="$("$SRC/config.guess")" \
        --host=m68k-unknown-elf \
        --prefix=/usr \
        --disable-shared --enable-static \
        --disable-assembly \
        --without-readline \
        CC="$CROSS_CC -std=gnu17 $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        CC_FOR_BUILD=cc \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -30 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
make -C "$BUILD" install DESTDIR="$BUILD/inst" > "$BUILD/install.log" 2>&1 \
    || { tail -20 "$BUILD/install.log"; exit 1; }
mkdir -p "$OUT"
cp -a "$BUILD/inst/usr/lib" "$BUILD/inst/usr/include" "$OUT/"
rm -rf "$OUT/lib/pkgconfig"
# THE LIBTOOL .la FILES HAVE TO GO. They record the library's install
# path as an absolute /usr/lib, which on this host is the HOST's
# /usr/lib -- so the next package's libtool follows libgmp.la there,
# finds something that is not a libtool archive at all, and stops with
# "'/usr/lib/libgmp.la' is not a valid libtool archive". Nothing reads
# them except libtool, and every consumer here is given an explicit
# --with-gmp=/--with-mpfr= path instead.
rm -f "$OUT/lib"/*.la

echo "gmp $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" -t "$OUT/lib/libgmp.a" 2>/dev/null | tail -1
