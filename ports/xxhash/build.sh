#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - xxHash: the library, for rsync's checksums, and xxhsum.
#
# One C file and a header. rsync negotiates xxh128, xxh3 and xxh64 for
# its block and whole-file checksums before MD5, and used to be built
# without them for want of this. xxhsum is built as well, because it is
# what proves the library right on a big-endian machine: kernel/
# xxhtest.sh compares its digests of the same files, for each algorithm,
# with the host's xxhsum built from the same source.
#
# Nothing built lands in this directory.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=0.8.3
# Pinned to the bytes first fetched: GitHub's generated tarball, with no
# signature published beside it.
SHA256=aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80
URL=https://github.com/Cyan4973/xxHash/archive/refs/tags/v$VERSION.tar.gz
SRC=$SRCDIR/xxHash-$VERSION
BUILD=$SRCDIR/build-xxhash-sage040
OUT=$BUILD/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/xxHash-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

libc_fresh "$BUILD" || true
mkdir -p "$BUILD" "$OUT/lib" "$OUT/include" "$OUT/bin"
CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS"
if [ ! -f "$OUT/lib/libxxhash.a" ] || [ "$SRC/xxhash.c" -nt "$OUT/lib/libxxhash.a" ]; then
    $CC -c "$SRC/xxhash.c" -o "$BUILD/xxhash.o"
    rm -f "$OUT/lib/libxxhash.a"
    "$CROSS_BIN/m68k-elf-ar" rcs "$OUT/lib/libxxhash.a" "$BUILD/xxhash.o"
fi
cp "$SRC/xxhash.h" "$SRC/xxh3.h" "$OUT/include/"
if [ ! -f "$OUT/bin/xxhsum" ] || [ "$OUT/lib/libxxhash.a" -nt "$OUT/bin/xxhsum" ]; then
    "$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS -I"$SRC" \
        "$SRC"/cli/xxhsum.c "$SRC"/cli/xsum_os_specific.c "$SRC"/cli/xsum_output.c \
        "$SRC"/cli/xsum_sanity_check.c "$SRC"/cli/xsum_bench.c \
        "$OUT/lib/libxxhash.a" -o "$OUT/bin/xxhsum" > "$BUILD/xxhsum.log" 2>&1 ||
        { tail -20 "$BUILD/xxhsum.log"; exit 1; }
    "$CROSS_BIN/m68k-elf-strip" "$OUT/bin/xxhsum"
fi
write_pc libxxhash "$VERSION" -lxxhash

echo "xxHash $VERSION -> $OUT"
