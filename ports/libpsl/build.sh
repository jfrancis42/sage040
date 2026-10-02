#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - libpsl for SuckOS: the Public Suffix List, and the `psl`
# command.
#
# The list says which parts of a name are a "public suffix" -- .com,
# .co.uk, .github.io -- so that a cookie set by one site cannot be
# scoped to every site under the same suffix. curl and wget both ask it
# before accepting a cookie's Domain; without it curl falls back to a
# much weaker rule.
#
# --enable-builtin: the list shipped in the release tarball is compiled
# into the library (as a DAFSA, made by a Python script on the HOST
# while building), so nothing has to be on the machine's disk and there
# is no file to go stale separately from the program.
#
# --enable-runtime=libidn2: international names in the list and in the
# question are compared in the same form, through ports/libidn2, which
# in turn needs ports/libunistring.
#
# Checked against Tim Rühsen's signature on the release.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=0.23.3
SHA256=93941f85a1e7bd593fa94f299233cb5dfc91cd144fd9a78a6ceb75001c5b03be
URL=https://github.com/rockdaboot/libpsl/releases/download/$VERSION/libpsl-$VERSION.tar.gz
SRC=$SRCDIR/libpsl-$VERSION
BUILD=$SRCDIR/build-libpsl-sage040
UNIOUT=$SRCDIR/build-libunistring-sage040/sage040
IDNOUT=$SRCDIR/build-libidn2-sage040/sage040

for p in libunistring libidn2; do
    "$HERE/../$p/build.sh" > /dev/null || {
        echo "libpsl: ports/$p/build.sh failed" >&2; exit 1; }
done

if [ ! -f "$SRC/configure" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/libpsl-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    if command -v gpg > /dev/null; then
        sig=$tarball.sig
        [ -f "$sig" ] || curl -sL --fail -o "$sig" "$URL.sig" || true
        if [ -f "$sig" ]; then
            gpg --verify "$sig" "$tarball" 2>&1 \
                | grep -q '^gpg: Good signature' \
                && echo "libpsl: good signature" \
                || echo "libpsl: signature NOT verified (no key?)" >&2
        fi
    fi
    tar -C "$SRCDIR" -xzf "$tarball"
fi

# See ports/libunistring/build.sh.
export gt_cv_func_uselocale_works=no

# pkg-config must EXIST -- configure stops without it -- but must not
# find the HOST's libidn2. So it is pointed at an empty directory and
# the answer is given directly: PKG_CHECK_MODULES takes LIBIDN2_CFLAGS
# and LIBIDN2_LIBS from the environment without asking pkg-config.
libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host="$HOST_TRIPLET" \
        --build="$("$SRC/build-aux/config.guess")" \
        --prefix=/usr \
        --disable-shared --enable-static \
        --enable-runtime=libidn2 --enable-builtin \
        --disable-man --disable-gtk-doc \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        --with-libunistring-prefix="$UNIOUT" \
        PKG_CONFIG="$(command -v pkg-config)" \
        PKG_CONFIG_LIBDIR=/nonexistent \
        LIBIDN2_CFLAGS="-I$IDNOUT/include" \
        LIBIDN2_LIBS="-L$IDNOUT/lib -lidn2 -L$UNIOUT/lib -lunistring" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

# The library and the command; not tests/ or fuzz/.
rm -f "$BUILD/make.log"
for d in include src tools; do
    [ -d "$BUILD/$d" ] || continue
    make -C "$BUILD/$d" -j"$(nproc)" >> "$BUILD/make.log" 2>&1 \
        || { tail -30 "$BUILD/make.log"; exit 1; }
done

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/bin"
cp "$BUILD/src/.libs/libpsl.a" "$OUT/lib/"
cp "$BUILD/include/libpsl.h" "$OUT/include/"
cp "$BUILD/tools/psl" "$OUT/bin/psl"

write_pc libpsl "$VERSION" -lpsl libidn2

echo "libpsl $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/psl" | tail -1
