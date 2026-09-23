#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - XZ Utils for SuckOS: liblzma, and the xz program.
#
# CPython's lzma module wants the library. The program reads and writes
# .xz, which is what a great deal of source arrives in.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=5.6.3
SHA256=b1d45295d3f71f25a4c9101bd7c8d16cb56348bbef3bbc738da0351e17c73317
URL=https://github.com/tukaani-project/xz/releases/download/v$VERSION/xz-$VERSION.tar.gz
SRC=$SRCDIR/xz-$VERSION
BUILD=$SRCDIR/build-xz-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/xz-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    # --disable-threads: xz's threading wants pthread_condattr_setclock
    # with CLOCK_MONOTONIC and a few other things; single-threaded
    # compression is what a 25 MHz machine does anyway.
    (cd "$BUILD" && cross_configure "$SRC" \
        --disable-shared --enable-static \
        --disable-threads --disable-nls --disable-doc \
        --disable-xzdec --disable-lzmadec --disable-lzmainfo \
        > configure.log 2>&1) || { tail -25 "$BUILD/configure.log"; exit 1; }
fi

# THE LIBRARY ALONE, not `all`.
#
# `all` also generates liblzma.pc, with a sed whose delimiter is a
# comma -- and our link line ("-Wl,--start-group -lc ...") is full of
# them, so the substitution ends in the middle and sed says "unknown
# option to 's'". Nothing here reads a pkg-config file; building the
# library target skips it.
make -C "$BUILD/src/liblzma" -j8 liblzma.la \
    > "$BUILD/make.log" 2>&1 || { tail -25 "$BUILD/make.log"; exit 1; }
make -C "$BUILD/src/xz" -j8 LDFLAGS="$DYN_LDFLAGS" LIBS="$DYN_LIBS" xz \
    >> "$BUILD/make.log" 2>&1 || true

OUT=$BUILD/sage040
mkdir -p "$OUT/lib" "$OUT/include/lzma" "$OUT/bin"
cp "$BUILD/src/liblzma/.libs/liblzma.a" "$OUT/lib/"
cp "$SRC/src/liblzma/api/lzma.h" "$OUT/include/"
cp "$SRC"/src/liblzma/api/lzma/*.h "$OUT/include/lzma/"
[ -x "$BUILD/src/xz/xz" ] && cp "$BUILD/src/xz/xz" "$OUT/bin/xz"

echo "xz $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/lib/liblzma.a" | tail -1
