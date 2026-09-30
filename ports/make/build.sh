#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU make for SuckOS.
#
# The machine had sbase's make, which is POSIX make and no more. That is
# enough for the Makefiles in this tree and not for the Makefiles the
# rest of the world writes: Perl's MakeMaker generates one that sbase's
# make stops on ("garbage at the end of the line"), and so does nearly
# every automake. GNU make is what `make` means on Linux, so it is what
# `make` means here: `make install` puts it at /bin/make, over sbase's
# (sbasetest still tests sbase's, on a disk of its own).
#
# Built without Guile and without its loadable-object support, which
# wants dlopen of objects made for a make on the same machine -- there
# is no call for it, and it is one fewer way to fail.
#
# Nothing built lands in this directory: it all goes to the build tree
# under ~/m68k/src, and `make install` copies it onto the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=4.4.1
# The same bytes as last time, and the checksum GNU's announcement
# published; the release signature's key is not on this workstation.
SHA256=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
URL=https://ftp.gnu.org/gnu/make/make-$VERSION.tar.gz
SRC=$SRCDIR/make-$VERSION
BUILD=$SRCDIR/build-make-sage040
OUT=$BUILD/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/make-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

libc_fresh "$BUILD" || true   # reconfigured if picolibc's headers changed
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    # -std=gnu17: its fnmatch.c declares `char *getenv ();`, which is
    # "no arguments" under gcc 15's C23 and then conflicts with <stdlib.h>.
    (cd "$BUILD" && cross_configure "$SRC" --disable-nls --without-guile \
        --disable-load --disable-dependency-tracking \
        CFLAGS="$CROSS_CFLAGS -std=gnu17" > configure.log 2>&1) \
        || { tail -30 "$BUILD/configure.log"; exit 1; }
fi
# gnulib's lib/ first: the `make` target does not name it as something
# to build, only as something to link.
{ make -C "$BUILD/lib" -j"$(nproc)" &&
  make -C "$BUILD" -j"$(nproc)" LDFLAGS="$DYN_LDFLAGS" LIBS="$DYN_LIBS" make; } \
    > "$BUILD/make.log" 2>&1 || { grep -E 'error|Error' "$BUILD/make.log" | head -20; exit 1; }

mkdir -p "$OUT/bin"
cp "$BUILD/make" "$OUT/bin/make"
"$CROSS_BIN/m68k-elf-strip" "$OUT/bin/make"
echo "make $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/make" | tail -1
