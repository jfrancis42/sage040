#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - zlib for SuckOS.
#
# Small, and needed by more than it looks: CPython's binascii, zipimport
# and gzip all want it, and so does anything that reads a compressed
# file. Nothing built lands in this directory -- it goes to the build
# tree under ~/m68k/src, where ports/python picks it up.
#
# zlib's configure is hand-written rather than autoconf's, so it takes
# the cross compiler through the environment instead of --host.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.3.1
SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
URL=https://zlib.net/fossils/zlib-$VERSION.tar.gz
SRC=$SRCDIR/zlib-$VERSION
BUILD=$SRCDIR/build-zlib-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/zlib-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

libc_fresh "$BUILD" || true
mkdir -p "$BUILD"

# Built from a copy of the source: zlib's configure writes into the
# source tree, so an out-of-tree build is not one of its options.
rsync -a --delete "$SRC/" "$BUILD/src/" 2>/dev/null || {
    rm -rf "$BUILD/src"; cp -r "$SRC" "$BUILD/src"; }

(cd "$BUILD/src" && \
    CC="$CROSS_CC" \
    AR="$CROSS_BIN/m68k-elf-ar" \
    RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
    CFLAGS="$CROSS_CFLAGS $CROSS_CPPFLAGS" \
    ./configure --static --prefix="$BUILD/sage040" > configure.log 2>&1)

make -C "$BUILD/src" -j8 libz.a > "$BUILD/make.log" 2>&1
mkdir -p "$BUILD/sage040/lib" "$BUILD/sage040/include"
cp "$BUILD/src/libz.a" "$BUILD/sage040/lib/"
cp "$BUILD/src/zlib.h" "$BUILD/src/zconf.h" "$BUILD/sage040/include/"

echo "zlib $VERSION -> $BUILD/sage040"
"$CROSS_BIN/m68k-elf-size" "$BUILD/sage040/lib/libz.a" | tail -n +2 |
    awk '{ t += $1 } END { printf "  %d bytes of text\n", t }'
