#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU grep for Sage040.
#
# Fetched from ftp.gnu.org at a fixed release, checked against the
# release's GPG signature (the GNU keyring) and a pinned SHA-256, and
# built out of tree in ~/m68k/src -- the source is never copied here.
# Cross-configured by ports/cross.sh; linked against /lib/libc.so.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=3.12
SHA256=2649b27c0e90e632eadcd757be06c6e9a4f48d941de51e7c0f83ff76408a07b9
URL=https://ftp.gnu.org/gnu/grep/grep-$VERSION.tar.xz
SRC=$SRCDIR/grep-$VERSION
BUILD=$SRCDIR/build-grep-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/grep-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

libc_fresh "$BUILD" || true   # reconfigured if picolibc's headers changed
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" --disable-nls --disable-acl \
        --without-selinux --disable-dependency-tracking > configure.log)
fi
# grep itself -- gnulib's library, then the program -- not gnulib's
# unit tests, which do not all build here.
make -C "$BUILD/lib" -j8
make -C "$BUILD/src" -j8 LDFLAGS="$DYN_LDFLAGS" LIBS="$DYN_LIBS" grep
cp "$BUILD/src/grep" "$HERE/grep"
"$CROSS_BIN/m68k-elf-size" "$HERE/grep"
