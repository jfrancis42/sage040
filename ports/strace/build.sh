#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - strace for SuckOS.
#
# What a program asked the kernel for and what it got: the answer to
# most porting failures here, which have tended to surface as some
# unrelated error from deep inside a large program. strace drives the
# kernel's ptrace (kernel/ptrace.c) exactly as on Linux/m68k --
# PTRACE_GETREGS for the registers, ORIG_D0 for the call -- and its
# tables of system calls are Linux/m68k's, which are this kernel's.
#
# Configured as --host=m68k-linux-gnu, not this tree's m68k-unknown-elf:
# strace builds for Linux and nothing else, and picks its m68k tables
# from the triplet.
#
# LINKED WITH MUSL, statically (ports/musl), not with picolibc: strace
# decodes the kernel's raw arguments and needs the C library's constants
# to be Linux's -- clocks, AT_ and SIGEV_ flags, siginfo's layout --
# which picolibc's deliberately are not. With picolibc it stopped on
# twenty static assertions that the two agreed, and patched past them it
# would have printed some arguments wrong. Built against musl and
# Linux's own m68k userspace headers (ports/linux-headers), and without the optional
# libraries -- libdw and libunwind for stack traces, libiberty for
# demangling, libselinux -- and without its "multiple personalities",
# which m68k has only one of.
#
# Nothing built lands in this directory: `make install` copies it onto
# the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=7.2
SHA256=4bde6246926890dcee824f6e6ac42a06752f47d77e5097d86e3c0d6d4b709fe5
URL=https://github.com/strace/strace/releases/download/v$VERSION/strace-$VERSION.tar.xz
SRC=$SRCDIR/strace-$VERSION
BUILD=$SRCDIR/build-strace-sage040
OUT=$BUILD/sage040

"$HERE/../musl/build.sh" > /dev/null
MUSLCC=$SRCDIR/build-musl-sage040/sage040/bin/m68k-sage040-musl-gcc

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/strace-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" --host=m68k-linux-gnu \
        --build="$("$SRC/build-aux/config.guess")" --prefix=/usr \
        --enable-mpers=no --disable-gcc-Werror --enable-bundled=yes \
        --without-libdw --without-libunwind --without-libiberty \
        --without-libselinux \
        CC="$MUSLCC" CFLAGS="-O2" \
        AR="$CROSS_BIN/m68k-elf-ar" RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi
make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { grep -E 'error|Error' "$BUILD/make.log" | head -20; exit 1; }

mkdir -p "$OUT/bin"
cp "$BUILD/src/strace" "$OUT/bin/strace"
"$CROSS_BIN/m68k-elf-strip" "$OUT/bin/strace"
echo "strace $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/strace" | tail -1
