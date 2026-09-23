#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - bzip2 for SuckOS: libbz2, and the program.
#
# CPython's bz2 module wants the library; the program is worth having on
# its own, because a .bz2 is a thing somebody hands you.
#
# bzip2 has a hand-written Makefile rather than a configure, so the
# cross compiler goes in through the environment.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.0.8
SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
URL=https://sourceware.org/pub/bzip2/bzip2-$VERSION.tar.gz
SRC=$SRCDIR/bzip2-$VERSION
BUILD=$SRCDIR/build-bzip2-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/bzip2-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

# Built from a copy: bzip2's Makefile builds in its own directory.
libc_fresh "$BUILD" || true
if [ ! -d "$BUILD/src" ]; then
    mkdir -p "$BUILD"
    cp -r "$SRC" "$BUILD/src"
fi

make -C "$BUILD/src" -j8 \
    CC="$CROSS_CC" AR="$CROSS_BIN/m68k-elf-ar" \
    RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
    CFLAGS="$CROSS_CFLAGS $CROSS_CPPFLAGS -D_FILE_OFFSET_BITS=64 -Winline" \
    libbz2.a > "$BUILD/make.log" 2>&1

# The program, linked against /lib/libc.so like every other here.
# bzip2's own Makefile links with $(CC) and no way to pass link flags,
# so it is linked here instead.
# shellcheck disable=SC2086
"$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS -D_FILE_OFFSET_BITS=64 \
    $DYN_LDFLAGS -I"$BUILD/src" \
    "$BUILD/src/bzip2.c" "$BUILD/src/libbz2.a" $DYN_LIBS \
    -o "$BUILD/bzip2" 2>> "$BUILD/make.log"

OUT=$BUILD/sage040
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/bin"
cp "$BUILD/src/libbz2.a" "$OUT/lib/"
cp "$BUILD/src/bzlib.h" "$OUT/include/"
cp "$BUILD/bzip2" "$OUT/bin/bzip2"

echo "bzip2 $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/bzip2" | tail -1
