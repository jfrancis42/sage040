#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - Linux's userspace headers for m68k: linux/*.h and asm/*.h.
#
# Not the kernel -- this system has its own -- but the description of
# Linux's interface, which is this kernel's interface too: the same
# system call numbers, the same structures, the same ioctls. A program
# that talks to the kernel directly rather than through the C library
# (strace is the one that needs them) is built against these.
#
# They come from a Linux release by `make headers_install ARCH=m68k`,
# which is the kernel's own way of producing them -- cleaned of
# everything kernel-internal. The release is chosen by what reads them:
# at least as new as strace's own idea of the interface, or its tables
# name constants (CLONE_AUTOREAP, btrfs's remap tree) the headers have
# not heard of. 6.18 was not.
#
# They go in their own directory, NOT among the C library's headers:
# picolibc has its own <asm/cachectl.h>, and nothing else should start
# finding a <linux/...> it did not ask for.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=7.2.8
SHA256=12e8d5a973d1ad7c5a5c69882e4022b131ed715db7003fdcd760ddf8c3e51941
URL=https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-$VERSION.tar.xz
BUILD=$SRCDIR/build-linux-headers-sage040
OUT=$BUILD/sage040

tarball=$SRCDIR/linux-$VERSION.tar.xz
[ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
echo "$SHA256  $tarball" | sha256sum -c - > /dev/null

if [ ! -f "$OUT/include/asm/unistd.h" ] || [ "$(cat "$OUT/.version" 2>/dev/null)" != "$VERSION" ]; then
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
    tar -C "$BUILD" -xf "$tarball"
    make -C "$BUILD/linux-$VERSION" ARCH=m68k headers_install \
        INSTALL_HDR_PATH="$OUT" > "$BUILD/headers.log" 2>&1 \
        || { tail -20 "$BUILD/headers.log"; exit 1; }
    rm -rf "$BUILD/linux-$VERSION"      # 1.5 GB of source, done with
    echo "$VERSION" > "$OUT/.version"
fi
echo "linux headers $VERSION (m68k) -> $OUT/include"
