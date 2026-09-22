#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU sed for Sage040.
#
# Fetched from ftp.gnu.org at a fixed release, checked against the
# release's GPG signature (the GNU keyring) and a pinned SHA-256, and
# built out of tree in ~/m68k/src -- the source is never copied here.
# Cross-configured by ports/cross.sh; linked against /lib/libc.so.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=4.10
SHA256=b8e72182b2ec96a3574e2998c47b7aaa64cc20ce000d8e9ac313cc07cecf28c7
URL=https://ftp.gnu.org/gnu/sed/sed-$VERSION.tar.xz
SRC=$SRCDIR/sed-$VERSION
BUILD=$SRCDIR/build-sed-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/sed-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" --disable-nls --disable-acl \
        --without-selinux --disable-dependency-tracking > configure.log)
    # sed 4.10's gnulib asks for mbrtoc32 "regular" -- a workaround for
    # a glibc 2.12 bug -- which calls mbszero(), a module sed does not
    # include; picolibc's mbrtoc32 is replaced here, so the path is taken
    # and does not compile. Nothing here has that glibc bug.
    sed -i 's|^#define GNULIB_MBRTOC32_REGULAR 1|/* GNULIB_MBRTOC32_REGULAR: see ports/sed/build.sh */|' \
        "$BUILD/config.h"
fi
# sed itself, not gnulib's unit tests, which do not all build here. The
# generated headers first: automake makes BUILT_SOURCES only for `all`,
# and without them a file that needs gnulib's <unistd.h> can compile
# before it exists -- a build that failed one time in two.
printf 'include Makefile\nsage040-built: $(BUILT_SOURCES)\n' > "$BUILD/sage040.mk"
make -C "$BUILD" -f sage040.mk -j8 sage040-built
make -C "$BUILD" -j8 LDFLAGS="$DYN_LDFLAGS" LIBS="$DYN_LIBS" sed/sed
cp "$BUILD/sed/sed" "$HERE/sed"
"$CROSS_BIN/m68k-elf-size" "$HERE/sed"
