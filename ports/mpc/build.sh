#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - MPC for SuckOS: complex arithmetic over MPFR.
#
# The third of gcc's three arithmetic libraries. gcc folds COMPLEX
# constant expressions with it, to the same standard of exactness that
# MPFR gives the real ones -- so `_Complex double z = 1.0/3.0 + I;`
# means the same thing from a cross compiler and a native one.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.2.1
SRC=$SRCDIR/mpc-$VERSION
BUILD=$SRCDIR/build-mpc-sage040
GMPOUT=$SRCDIR/build-gmp-sage040/sage040
MPFROUT=$SRCDIR/build-mpfr-sage040/sage040


# Fetched if it is not here. The version is pinned and the tarball is
# checked, so this cannot pick up a different one -- which was the
# reason these used to refuse to fetch, and the reason the native
# toolchain could only be built on a machine that already had the
# source lying around from some earlier build.
#
# Checksum of the release verified against the GNU keyring:
# "Good signature from Andreas Enge". (Some of these signing keys have since
# EXPIRED; a key expiring after it signed does not unmake the
# signature, and the SHA-256 below is what is actually enforced here.)
# Change VERSION and you must change SHA256 with it.
URL=https://ftp.gnu.org/gnu/mpc/mpc-$VERSION.tar.gz
SHA256=17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/mpc-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

[ -f "$GMPOUT/lib/libgmp.a" ] || "$HERE/../gmp/build.sh"
[ -f "$MPFROUT/lib/libmpfr.a" ] || "$HERE/../mpfr/build.sh"

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --build="$("$SRC/config.guess" 2>/dev/null || cc -dumpmachine)" \
        --host=m68k-unknown-elf \
        --prefix=/usr \
        --disable-shared --enable-static \
        --with-gmp="$GMPOUT" \
        --with-mpfr="$MPFROUT" \
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

echo "mpc $VERSION -> $OUT"
