#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU libunistring for SuckOS: Unicode strings in C --
# conversion, normalisation, case mapping, character properties.
#
# A LIBRARY ONLY, and a dependency: libidn2 (international domain
# names) and libpsl (the public suffix list) need it, and wget and curl
# need those. libidn2 can carry a private subset of it instead, but
# libpsl cannot, so there is one copy, here.
#
# --disable-shared: everything that links it does so statically, as with
# every library in ports/ except libc itself.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.4.2
SHA256=5b46e74377ed7409c5b75e7a96f95377b095623b689d8522620927964a41499c
URL=https://ftp.gnu.org/gnu/libunistring/libunistring-$VERSION.tar.xz
SRC=$SRCDIR/libunistring-$VERSION
BUILD=$SRCDIR/build-libunistring-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/libunistring-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    if command -v gpg > /dev/null; then
        sig=$tarball.sig
        ring=$SRCDIR/gnu-keyring.gpg
        [ -f "$sig" ] || curl -sL --fail -o "$sig" "$URL.sig" || true
        [ -f "$ring" ] || \
            curl -sL --fail -o "$ring" https://ftp.gnu.org/gnu/gnu-keyring.gpg || true
        if [ -f "$sig" ] && [ -f "$ring" ]; then
            gpg --no-default-keyring --keyring "$ring" \
                --verify "$sig" "$tarball" 2>&1 | grep -q '^gpg: Good signature' \
                || { echo "libunistring: BAD signature" >&2; exit 1; }
            echo "libunistring: good signature (Bruno Haible, GNU keyring)"
        fi
    fi
    tar -C "$SRCDIR" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# The same answer ports/gcc and ports/gettext give: this gnulib decides
# uselocale() is usable from a compile test and then calls
# uselocale(NULL), which picolibc's locale_t is not a pointer for.
export gt_cv_func_uselocale_works=no

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host="$HOST_TRIPLET" \
        --build="$("$SRC/build-aux/config.guess")" \
        --prefix=/usr \
        --disable-shared --enable-static \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

# lib/ only. The tests/ directory is gnulib's own test suite, built for
# the machine and never run here, and it wants a pselect() replacement
# that does not compile against picolibc.
make -C "$BUILD/lib" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -30 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT" "$BUILD/inst"
# install -p keeps each header's own date, so rebuilding this library
# does not make everything built against it look out of date.
make -C "$BUILD/lib" install DESTDIR="$BUILD/inst" INSTALL="$(command -v install) -p" > "$BUILD/install.log" 2>&1 \
    || { tail -20 "$BUILD/install.log"; exit 1; }
mkdir -p "$OUT"
cp -a "$BUILD/inst/usr/lib" "$BUILD/inst/usr/include" "$OUT/"
rm -f "$OUT"/lib/*.la

echo "libunistring $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" -t "$OUT/lib/libunistring.a" | tail -1
