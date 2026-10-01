#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU patch for SuckOS.
#
# Applying a patch on the machine is the other half of porting there
# (the first is ports/diffutils). Every port in this tree is a release
# plus a patches/ directory.
#
# Nothing built lands in this directory: it all goes to the build tree
# under ~/m68k/src, and `make install` copies it onto the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=2.8
# The same bytes as last time; the release signature's key is not on
# this workstation.
SHA256=f87cee69eec2b4fcbf60a396b030ad6aa3415f192aa5f7ee84cad5e11f7f5ae3
URL=https://ftp.gnu.org/gnu/patch/patch-$VERSION.tar.xz
SRC=$SRCDIR/patch-$VERSION
BUILD=$SRCDIR/build-patch-sage040
OUT=$BUILD/sage040
PROGS="patch"

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/patch-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

libc_fresh "$BUILD" || true   # reconfigured if picolibc's headers changed
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    # --prefix=/usr because patch goes to /usr/bin.
    # The same two questions as ports/diffutils (the same gnulib asks
    # them), and the same answers for the same reasons: strcasecmp
    # works, and getopt is called not POSIX so that gnulib's own is used.
    (cd "$BUILD" && cross_configure "$SRC" --prefix=/usr --disable-nls \
        --disable-dependency-tracking \
        gl_cv_func_strcasecmp_works=yes gl_cv_func_getopt_posix=no \
        > configure.log 2>&1) \
        || { tail -30 "$BUILD/configure.log"; exit 1; }
fi
{ make -C "$BUILD/lib" -j"$(nproc)" &&
  make -C "$BUILD/src" -j"$(nproc)" LDFLAGS="$DYN_LDFLAGS" LIBS="$DYN_LIBS" $PROGS; } \
    > "$BUILD/make.log" 2>&1 || { grep -E 'error|Error' "$BUILD/make.log" | head -20; exit 1; }

mkdir -p "$OUT/bin"
for p in $PROGS; do
    cp "$BUILD/src/$p" "$OUT/bin/$p"
    "$CROSS_BIN/m68k-elf-strip" "$OUT/bin/$p"
done
echo "patch $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT"/bin/* | tail -n +2
