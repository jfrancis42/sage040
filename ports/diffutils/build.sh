#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU diffutils for SuckOS: diff, cmp, diff3 and sdiff.
#
# sbase has no diff, and diff is half of porting on the machine (the
# other half is ports/patch): bash's, sed's and grep's own test suites
# compare output with it, and git runs it.
#
# Nothing built lands in this directory: it all goes to the build tree
# under ~/m68k/src, and `make install` copies it onto the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=3.12
# The same bytes as last time; the release signature's key is not on
# this workstation.
SHA256=7c8b7f9fc8609141fdea9cece85249d308624391ff61dedaf528fcb337727dfd
URL=https://ftp.gnu.org/gnu/diffutils/diffutils-$VERSION.tar.xz
SRC=$SRCDIR/diffutils-$VERSION
BUILD=$SRCDIR/build-diffutils-sage040
OUT=$BUILD/sage040
PROGS="diff cmp diff3 sdiff"

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/diffutils-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

libc_fresh "$BUILD" || true   # reconfigured if picolibc's headers changed
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    # --prefix=/usr because the programs go to /usr/bin. (diff3 and sdiff
    # find diff through PATH: src/paths.h says plain "diff".)
    # Two questions configure can only answer by running a program.
    # strcasecmp works; getopt is called not POSIX so that gnulib's own,
    # which is, is used rather than picolibc's, which nothing has tested.
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
echo "diffutils $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT"/bin/* | tail -n +2
