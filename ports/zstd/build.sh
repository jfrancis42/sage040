#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - Zstandard for SuckOS: the library, and the command.
#
# CPython 3.14 has a `compression.zstd` module in the standard library --
# it is no longer optional the way lzma feels optional -- and it wants
# libzstd. The command is worth having on its own: it is the one
# compressor here that is fast enough to use without thinking about it.
#
# SINGLE-THREADED ON PURPOSE. libzstd's multi-threaded compressor is
# built only when ZSTD_MULTITHREAD is defined; leaving it undefined is
# what this machine wants anyway -- one core, and a worker pool would
# cost memory it has better uses for.
#
# No assembly: the only hand-written assembly in zstd is x86-64's
# Huffman decoder, and the build turns it off for every other target by
# itself.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.5.7
# Fetched over TLS from the project's own release URL and pinned here.
# Facebook publishes no hash for the release tarball anywhere I can
# check it against, so this pin says "the same bytes as last time",
# not "the bytes the authors signed" -- unlike readline, whose
# signature is checked against GNU's keyring.
SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
URL=https://github.com/facebook/zstd/releases/download/v$VERSION/zstd-$VERSION.tar.gz
SRC=$SRCDIR/zstd-$VERSION
BUILD=$SRCDIR/build-zstd-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/zstd-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

libc_fresh "$BUILD" || true
mkdir -p "$BUILD"

# zstd's Makefiles build in the source tree. Copy what is needed so a
# host build of zstd in the same tree cannot be confused with this one,
# and so `make clean` here never touches the pristine source.
#
# ONCE per source tree, not on every run: the objects live inside the
# copy (src/lib/obj), so recopying threw them away and every `make` of
# the whole system recompiled zstd, twice over (its own build and
# curl's).
if [ "$(cat "$BUILD/src/.from" 2>/dev/null)" != "$SRC" ]; then
    rm -rf "$BUILD/src"
    mkdir -p "$BUILD/src"
    cp -a "$SRC/lib" "$SRC/programs" "$BUILD/src/"
    echo "$SRC" > "$BUILD/src/.from"
fi

MAKEVARS=(
    CC="$CROSS_CC"
    AR="$CROSS_BIN/m68k-elf-ar"
    RANLIB="$CROSS_BIN/m68k-elf-ranlib"
    CPPFLAGS="$CROSS_CPPFLAGS"
    CFLAGS="$CROSS_CFLAGS"
    # zstd's build runs the compiler to see which warnings it takes.
    # Those probes link, so they need a link line that works here.
    LDFLAGS="$STATIC_LDFLAGS"
    LIBS="$STATIC_LIBS"
    HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 HAVE_PTHREAD=0
    ZSTD_NO_ASM=1
    # The legacy decoders (v0.1-v0.7 frames) are built, as upstream
    # builds them. They used to be switched off: uint32_t was `long
    # unsigned int` here and the v0.7 decoder passes a U32 * for an
    # unsigned *. gcc's integer types are Linux's now (ports/gcc/
    # patches/02), uint32_t is unsigned int, and it compiles.
)

make -C "$BUILD/src/lib" -j8 "${MAKEVARS[@]}" libzstd.a \
    > "$BUILD/lib.log" 2>&1 || { tail -25 "$BUILD/lib.log"; exit 1; }

# The command, against /lib/libc.so. LDLIBS, not LIBS: zstd's programs
# Makefile links with `$(CC) $(FLAGS) $^ $(LDLIBS) -o $@` and never
# mentions LIBS, so the libraries went nowhere and every libc symbol
# was undefined at the link.
make -C "$BUILD/src/programs" -j8 "${MAKEVARS[@]}" \
    LDFLAGS="$DYN_LDFLAGS" LDLIBS="$DYN_LIBS" \
    zstd > "$BUILD/prog.log" 2>&1 || { tail -25 "$BUILD/prog.log"; exit 1; }

OUT=$BUILD/sage040
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/bin"
cp "$BUILD/src/lib/libzstd.a" "$OUT/lib/"
# zdict.h as well as zstd.h: the dictionary builder is part of the
# library (libzstd.a has the ZDICT_* symbols in it) and CPython's
# _zstd module includes <zdict.h> by name. Installing only zstd.h
# built a library whose header set could not compile its own user.
cp "$BUILD/src/lib/zstd.h" "$BUILD/src/lib/zstd_errors.h" \
   "$BUILD/src/lib/zdict.h" "$OUT/include/"
cp "$BUILD/src/programs/zstd" "$OUT/bin/zstd"

echo "zstd $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/zstd" | tail -1
