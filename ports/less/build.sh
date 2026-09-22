#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - less for SuckOS.
#
# The pager. It is the first program here that uses the terminal the way
# a full-screen program does -- raw mode, cursor addressing, the screen's
# size, and the terminal's own capabilities looked up in the terminfo
# database (ports/ncurses) rather than assumed.
#
# Linked against libtinfow, not libncursesw: less asks terminfo what the
# terminal can do and writes the sequences itself. It has no use for
# curses' windows, and libtinfo is the half of ncurses that answers that
# question.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=668
SHA256=2819f55564d86d542abbecafd82ff61e819a3eec967faa36cd3e68f1596a44b8
URL=https://www.greenwoodsoftware.com/less/less-$VERSION.tar.gz
SRC=$SRCDIR/less-$VERSION
BUILD=$SRCDIR/build-less-sage040
NCOUT=$SRCDIR/build-ncurses-sage040/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/less-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

if [ ! -f "$NCOUT/lib/libtinfow.a" ]; then
    "$HERE/../ncurses/build.sh"
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" \
        --with-regex=posix \
        CPPFLAGS="$CROSS_CPPFLAGS -I$NCOUT/include" \
        LDFLAGS="$STATIC_LDFLAGS -L$NCOUT/lib" \
        LIBS="-ltinfow $STATIC_LIBS" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j8 \
    LDFLAGS="$DYN_LDFLAGS -L$NCOUT/lib" \
    LIBS="-ltinfow $DYN_LIBS" \
    less lesskey > "$BUILD/make.log" 2>&1 || { tail -30 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
mkdir -p "$OUT/bin"
cp "$BUILD/less" "$OUT/bin/less"
[ -x "$BUILD/lesskey" ] && cp "$BUILD/lesskey" "$OUT/bin/lesskey"

echo "less $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/less"
