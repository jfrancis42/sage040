#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - Brotli for SuckOS: libbrotlicommon, libbrotlidec and
# libbrotlienc, and the `brotli` command.
#
# It is here for curl, which asks for "br" alongside gzip and zstd when
# built with it -- the encoding most web servers prefer for text.
#
# COMPILED DIRECTLY, not through Brotli's CMake: it is plain C with no
# configuration step that matters (byte order and unaligned access are
# decided by the preprocessor from what the compiler says), and a CMake
# cross toolchain file would be the only CMake in the tree.
#
# Byte order is the thing worth checking on this machine, and the
# library settles it from __BYTE_ORDER__, which m68k-elf-gcc defines as
# big-endian. kernel/brotlitest.sh (via curltest's round trip) is where
# that claim is measured rather than believed.
#
# There is no signed release: this is the v1.2.0 tag's archive from
# GitHub, pinned by its hash.

set -eu

cd "$(dirname "$0")"
. ../cross.sh

VERSION=1.2.0
SHA256=816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
URL=https://github.com/google/brotli/archive/refs/tags/v$VERSION.tar.gz
SRC=$SRCDIR/brotli-$VERSION
BUILD=$SRCDIR/build-brotli-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/brotli-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

libc_fresh "$BUILD" || true
mkdir -p "$BUILD/obj"

# shellcheck disable=SC2086
cc_one() {                      # cc_one SRC.c OBJ.o
    "$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS -I"$SRC/c/include" \
        -Wall -c "$1" -o "$2"
}

for part in common dec enc; do
    objs=
    for c in "$SRC/c/$part"/*.c; do
        o="$BUILD/obj/$part-$(basename "$c" .c).o"
        if [ ! -f "$o" ] || [ "$c" -nt "$o" ]; then
            cc_one "$c" "$o" 2>> "$BUILD/make.log" || {
                tail -20 "$BUILD/make.log"; exit 1; }
        fi
        objs="$objs $o"
    done
    rm -f "$BUILD/libbrotli$part.a"
    # shellcheck disable=SC2086
    "$CROSS_BIN/m68k-elf-ar" rcs "$BUILD/libbrotli$part.a" $objs
done

# The command, against /lib/libc.so.
# shellcheck disable=SC2086
"$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS -I"$SRC/c/include" $DYN_LDFLAGS \
    "$SRC/c/tools/brotli.c" \
    "$BUILD/libbrotlienc.a" "$BUILD/libbrotlidec.a" "$BUILD/libbrotlicommon.a" \
    $DYN_LIBS -o "$BUILD/brotli" 2>> "$BUILD/make.log" || {
    tail -20 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/bin"
cp "$BUILD"/libbrotli*.a "$OUT/lib/"
cp -r "$SRC/c/include/brotli" "$OUT/include/"
cp "$BUILD/brotli" "$OUT/bin/brotli"

write_pc libbrotlicommon "$VERSION" -lbrotlicommon
write_pc libbrotlidec "$VERSION" -lbrotlidec libbrotlicommon
write_pc libbrotlienc "$VERSION" -lbrotlienc libbrotlicommon

echo "brotli $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/brotli" | tail -1
