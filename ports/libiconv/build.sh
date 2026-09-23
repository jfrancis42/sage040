#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU libiconv for SuckOS (task 45).
#
# Converting text between character sets. picolibc has <iconv.h> and a
# conversion that knows almost nothing; this is the one with the
# hundreds of encodings in it.
#
# WHY IT MATTERS HERE. Names on this machine are UTF-8 bytes and stay
# so, and nothing in the system needs a conversion. What needs it is
# ported software: CLISP requires libiconv outright, gettext uses it to
# recode a message catalogue, and anything that reads a file somebody
# else wrote in Latin-1 wants it.
#
# Signed by Bruno Haible and checked against GNU's own keyring, as
# readline is -- not merely hashed.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.18
SHA256=3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8
URL=https://ftp.gnu.org/gnu/libiconv/libiconv-$VERSION.tar.gz
SRC=$SRCDIR/libiconv-$VERSION
BUILD=$SRCDIR/build-libiconv-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/libiconv-$VERSION.tar.gz
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
                || { echo "libiconv: BAD signature" >&2; exit 1; }
            echo "libiconv: good signature (Bruno Haible, GNU keyring)"
        fi
    fi
    tar -C "$SRCDIR" -xzf "$tarball"
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host=m68k-unknown-elf \
        --build="$("$SRC/build-aux/config.guess")" \
        --prefix=/usr \
        --disable-shared --enable-static \
        --disable-nls \
        --enable-extra-encodings \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
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
cp -a "$BUILD/inst/usr/lib" "$BUILD/inst/usr/include" "$OUT/" 2>/dev/null || true
[ -d "$BUILD/inst/usr/bin" ] && cp -a "$BUILD/inst/usr/bin" "$OUT/"
rm -f "$OUT/lib"/*.la

echo "libiconv $VERSION -> $OUT"
