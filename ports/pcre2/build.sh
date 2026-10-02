#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - PCRE2, the Perl-compatible regular expression library.
#
# For grep -P first, and for any later port that wants it. The 8-bit
# library only, static, with Unicode; no JIT, which has no m68k back
# end -- the interpreter is what runs, and it is the same matcher the
# JIT would have compiled. pcre2grep and pcre2test are built too:
# pcre2test is how kernel/pcretest.sh runs PCRE2's own test data on the
# machine and compares with the host.
#
# Nothing built lands in this directory.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=10.45
# Pinned to the release asset first fetched; the .sig beside it is
# checked when gpg has the maintainers' key.
SHA256=21547f3516120c75597e5b30a992e27a592a31950b5140e7b8bfde3f192033c4
URL=https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$VERSION/pcre2-$VERSION.tar.bz2
SRC=$SRCDIR/pcre2-$VERSION
BUILD=$SRCDIR/build-pcre2-sage040
OUT=$BUILD/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/pcre2-$VERSION.tar.bz2
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xjf "$tarball"
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" --prefix=/usr \
        --disable-shared --enable-static --disable-jit --enable-unicode \
        --enable-pcre2-8 --disable-pcre2-16 --disable-pcre2-32 \
        --disable-pcre2grep-libz --disable-pcre2grep-libbz2 \
        --disable-pcre2test-libreadline --disable-pcre2test-libedit \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi
make -C "$BUILD" -j"$(nproc)" libpcre2-8.la > "$BUILD/make.log" 2>&1 ||
    { tail -30 "$BUILD/make.log"; exit 1; }
make -C "$BUILD" -j"$(nproc)" LDFLAGS="$DYN_LDFLAGS" LIBS="$DYN_LIBS" \
    pcre2grep pcre2test >> "$BUILD/make.log" 2>&1 ||
    { tail -30 "$BUILD/make.log"; exit 1; }

mkdir -p "$OUT/lib/pkgconfig" "$OUT/include" "$OUT/bin"
cp "$BUILD/.libs/libpcre2-8.a" "$OUT/lib/"
cp "$BUILD/src/pcre2.h" "$OUT/include/" 2>/dev/null || cp "$BUILD/pcre2.h" "$OUT/include/"
sed -e "s|^prefix=.*|prefix=$OUT|" "$BUILD/libpcre2-8.pc" > "$OUT/lib/pkgconfig/libpcre2-8.pc"
for p in pcre2grep pcre2test; do
    cp "$BUILD/$p" "$OUT/bin/$p"
    "$CROSS_BIN/m68k-elf-strip" "$OUT/bin/$p"
done
echo "PCRE2 $VERSION -> $OUT"
