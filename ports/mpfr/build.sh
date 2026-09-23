#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - MPFR for SuckOS: correctly-rounded multiple-precision
# floating point, over GMP.
#
# gcc needs it to fold floating-point constant expressions to the
# answer the TARGET would produce, rather than the answer the host's
# double happens to give. That is what makes a cross compiler and a
# native one agree, and it is why gcc will not build without it.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=4.1.0
SRC=$SRCDIR/mpfr-$VERSION
BUILD=$SRCDIR/build-mpfr-sage040
GMPOUT=$SRCDIR/build-gmp-sage040/sage040

[ -d "$SRC" ] || { echo "mpfr: $SRC is not there" >&2; exit 1; }
[ -f "$GMPOUT/lib/libgmp.a" ] || "$HERE/../gmp/build.sh"

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --build="$("$SRC/config.guess" 2>/dev/null || cc -dumpmachine)" \
        --host=m68k-unknown-elf \
        --prefix=/usr \
        --disable-shared --enable-static \
        --with-gmp="$GMPOUT" \
        CC="$CROSS_CC -std=gnu17 $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        CC_FOR_BUILD=cc \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -30 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT" "$BUILD/inst"
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

echo "mpfr $VERSION -> $OUT"
