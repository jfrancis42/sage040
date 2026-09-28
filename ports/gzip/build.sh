#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU gzip for SuckOS: gzip, and the same program as gunzip
# and zcat.
#
# The machine had bzip2, xz and zstd and no gzip at all -- the one
# compression every script, tarball and web server assumes. It is also
# what Lynx looks for on PATH before it will decode a gzip-encoded page:
# Lynx links zlib, but decides whether it CAN decode gzip by finding a
# `gzip` program (HTGetProgramPath), so without one every compressed
# page came out as raw bytes.
#
# gzip decides what it is from its own name -- "gunzip" decompresses,
# "zcat" decompresses to stdout -- so the Makefile installs this one
# binary three times rather than gzip's wrapper shell scripts.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.15
SHA256=9aa0cc780dec156b8282844833b342ab7cb08c25d2cd9a1869cdd0df31deff48
URL=https://ftp.gnu.org/gnu/gzip/gzip-$VERSION.tar.xz
SRC=$SRCDIR/gzip-$VERSION
BUILD=$SRCDIR/build-gzip-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/gzip-$VERSION.tar.xz
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
                || { echo "gzip: BAD signature" >&2; exit 1; }
            echo "gzip: good signature (Jim Meyering, GNU keyring)"
        fi
    fi
    tar -C "$SRCDIR" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# See ports/libunistring/build.sh.
export gt_cv_func_uselocale_works=no

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host="$HOST_TRIPLET" \
        --build="$("$SRC/build-aux/config.guess")" \
        --prefix=/usr \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

# gnulib (lib/) and the program; not the tests or the documentation.
rm -f "$BUILD/make.log"
make -C "$BUILD/lib" -j"$(nproc)" >> "$BUILD/make.log" 2>&1 \
    || { tail -30 "$BUILD/make.log"; exit 1; }
# version.h is generated at the top level; gzip.c includes it.
make -C "$BUILD" -j"$(nproc)" version.h gzip >> "$BUILD/make.log" 2>&1 \
    || { tail -30 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$BUILD/gzip" "$OUT/bin/gzip"

echo "gzip $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/gzip" | tail -1
