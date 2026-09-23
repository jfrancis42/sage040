#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - libffi for SuckOS.
#
# libffi calls a function whose signature is only known at run time. It
# is what CPython's `ctypes` is built on, and the reason to have it here
# is that `_ctypes` then compiles -- structure layout, function
# pointers, callbacks through ctypes.CFUNCTYPE all work.
#
# BE CLEAR ABOUT WHAT IT DOES NOT BUY. ctypes' usual job is to open a
# shared library by name and call into it, and that needs dlopen, which
# this system's loader does not have: ld.so resolves what a program was
# linked against and stops. So `ctypes.CDLL("libm.so")` will not work
# however good libffi is. What remains -- calling a pointer the program
# already has, and making C-callable callbacks -- does.
#
# m68k is a target libffi supports itself (src/m68k/sysv.S); nothing
# here is a port.
#
# CLOSURES NEED EXECUTABLE MEMORY. libffi writes a small trampoline and
# jumps into it, so a closure only runs if the page it lands in is
# mapped executable. That is a property of this kernel's mmap, and the
# test below is what says whether it holds today rather than a guess
# made here.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=3.5.2
# Fetched over TLS from the project's own release URL. libffi publish
# no hash for it, so this pin means "unchanged since last time", not
# "signed by the authors".
SHA256=f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc
URL=https://github.com/libffi/libffi/releases/download/v$VERSION/libffi-$VERSION.tar.gz
SRC=$SRCDIR/libffi-$VERSION
BUILD=$SRCDIR/build-libffi-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/libffi-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" \
        --disable-shared --enable-static \
        --disable-docs \
        --disable-multi-os-directory \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j8 > "$BUILD/make.log" 2>&1 \
    || { tail -30 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
make -C "$BUILD" install DESTDIR="$BUILD/inst" \
    > "$BUILD/install.log" 2>&1 || { tail -20 "$BUILD/install.log"; exit 1; }
mkdir -p "$OUT"
cp -a "$BUILD/inst/usr/local/lib" "$BUILD/inst/usr/local/include" "$OUT/" \
    2>/dev/null || cp -a "$BUILD/inst"/*/lib "$BUILD/inst"/*/include "$OUT/"
rm -rf "$OUT/lib/pkgconfig"

echo "libffi $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" -t "$OUT/lib/libffi.a" 2>/dev/null | tail -1
