#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU binutils, to RUN ON the machine (task 49).
#
# as, ld, ar, ranlib, nm, objdump, objcopy, strip, readelf, size,
# strings, addr2line -- the assembler and linker the machine needs to
# build anything, including its own kernel.
#
# THIS IS A CANADIAN CROSS, and the three triplets are all different
# jobs:
#
#   --build   x86_64-linux: the machine doing the compiling. mother.
#   --host    m68k: where the RESULT runs. The Sage040.
#   --target  m68k: what the result produces code FOR. Also the
#             Sage040, which is what makes it a native toolchain
#             rather than a cross one that happens to run here.
#
# Nothing is compiled inside the emulator. The emulator's only job is
# to run what comes out, which is the test.
#
# WHAT IS SWITCHED OFF, and why each is a property of this system:
#
#   --disable-plugins   ld's plugin interface is dlopen, and this
#                       system's loader has none. Left on, ld builds
#                       code that cannot work
#   --disable-nls       gettext is not built for this machine yet, and
#                       binutils in English needs none of it
#   --disable-gdb       gdb is its own build: it is C++ and wants a
#                       C++ runtime this system does not have yet
#   --disable-gprofng   a profiler that wants Linux's perf machinery
#   --without-zstd      binutils would use it for compressed debug
#                       sections; the in-tree zlib covers the format
#                       anything here produces
#   --disable-werror    a 2025 compiler on a 2025 source tree finds
#                       warnings the release was not built with
#
# EVERY FLAG IS BAKED INTO $CC, which looks heavy-handed and is the
# only thing that works here. binutils configures a dozen
# subdirectories of its own (libiberty, zlib, bfd, opcodes, libctf,
# libsframe), and what reaches them is not what reaches the top:
#
#   - CFLAGS is passed down; CPPFLAGS is NOT. With the -isystem only in
#     CPPFLAGS, libiberty could not find <stdio.h>, concluded the
#     compiler could not produce executables at all, and every later
#     test failed with "Link tests are not allowed after
#     GCC_NO_EXECUTABLES" -- a message that says nothing about a
#     missing header.
#   - Moving them to CFLAGS fixed the compile tests and not the
#     PREPROCESSOR-ONLY ones, because autoconf runs those as
#     `$CPP $CPPFLAGS` with CPP="$CC -E" and no CFLAGS anywhere near
#     them. AC_HEADER_STDC is one: it came back "no", so libiberty
#     built regex.c and md5.c with no <string.h> and no <stdlib.h>,
#     and failed on "too many arguments to function 'malloc';
#     expected 0, have 1" -- an implicit declaration, three steps
#     downstream of the actual cause.
#
# Flags inside CC are in all three: compiling, preprocessing and
# linking. It is what the C library's own cross.sh comment warns
# about, met in a build system big enough to show both halves.
#
# ac_cv_tls=none BECAUSE THIS SYSTEM HAS NO THREAD-LOCAL STORAGE, and
# binutils cannot find that out for itself: its test COMPILES
# `static thread_local int bar;` and never links it. Compiling is fine
# -- the m68k back end emits a call to __m68k_read_tp, which is how a
# CPU with no thread pointer register does TLS -- and the link then
# fails on that symbol, which nothing here provides. bfd uses TLS for
# one variable, its current error code, and without it falls back to a
# plain global. That is what it does on every platform without TLS, and
# these tools run single-threaded here.
#
# Giving this system real __thread support is worth doing and is a
# subsystem of its own: PT_TLS in ld.so, a per-thread block, and
# __m68k_read_tp in the C library. It is written up in progress.md.
#
# zlib is binutils' own in-tree copy, deliberately: --with-system-zlib
# would point at ports/zlib, and there is nothing to gain from making
# the assembler depend on another port.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=2.45
SRC=$SRCDIR/binutils-$VERSION
BUILD=$SRCDIR/build-binutils-native
STAGE=$BUILD/stage

# The same source the CROSS binutils was built from.
#
# This used to refuse to fetch, on the reasoning that downloading
# something might get a DIFFERENT version from the one the cross tools
# were built from, and then the two would disagree about object
# formats. The reasoning is right and the conclusion was wrong: the
# version is pinned here and the tarball is checked against a SHA-256,
# so what is fetched cannot be a different version -- while refusing to
# fetch meant the native toolchain could only ever be built on the one
# machine that happened to still have the source lying around, and
# `make install` failed everywhere else.
#
# The checksum was taken from the release verified against the GNU
# keyring: "Good signature from Nick Clifton (Chief Binutils
# Maintainer)". Change VERSION and you must change SHA256 with it.
URL=https://ftp.gnu.org/gnu/binutils/binutils-$VERSION.tar.xz
SHA256=c50c0e7f9cb188980e2cc97e4537626b1672441815587f1eab69d2a1bfbef5d2

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/binutils-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

# Patches: each one a bug that only shows on a target where uint32_t
# is not `unsigned int`. See the head of each.
#
# Applied to the SHARED source tree, which the cross binutils was also
# built from -- so they are tracked, and applied once.
applied=$SRC/.sage040-patches
touch "$applied"
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    name=$(basename "$p")
    grep -qxF "$name" "$applied" && continue
    patch -d "$SRC" -p1 -N -s < "$p"
    echo "$name" >> "$applied"
done

HOST_TRIPLET=m68k-unknown-elf
BUILD_TRIPLET=$("$SRC/config.guess")

# EXPORTED FOR THE WHOLE SCRIPT, and it has to be. bfd, libiberty,
# opcodes and the rest each have a configure of their own, run not by
# the line below but by `make` further down -- so a cache variable set
# only around the top-level configure never reaches the subdirectory
# that needs it. Naming it as a configure ARGUMENT does not reach it
# either: the subdirectories are given a cache file of their own.
export ac_cv_tls=none

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --build="$BUILD_TRIPLET" \
        --host="$HOST_TRIPLET" \
        --target="$HOST_TRIPLET" \
        --prefix=/usr \
        --program-prefix= \
        --disable-nls \
        --disable-werror \
        --disable-plugins \
        --disable-gdb \
        --disable-gdbserver \
        --disable-sim \
        --disable-gprofng \
        --disable-shared \
        --enable-static \
        --without-zstd \
        --without-debuginfod \
        --disable-libdecnumber \
        --disable-readline \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        CC_FOR_BUILD=cc \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        CPPFLAGS="$CROSS_CPPFLAGS" \
        CFLAGS="$CROSS_CFLAGS" \
        > configure.log 2>&1) || { tail -40 "$BUILD/configure.log"; exit 1; }
fi

# THE SPECS FILE DOES THE LINKING, so there is no LDFLAGS/LIBS dance
# here and configure's own link tests work: a plain `gcc a.o b.o -o x`
# produces a dynamic program against /lib/libc.so, which is what these
# tools should be. Every other port in this tree passes the link line
# explicitly instead; they were written before the specs file existed
# and there is no reason to churn them.
make -C "$BUILD" -j"$(nproc)" \
    > "$BUILD/make.log" 2>&1 || { tail -40 "$BUILD/make.log"; exit 1; }

rm -rf "$STAGE"
make -C "$BUILD" install DESTDIR="$STAGE" \
    > "$BUILD/install.log" 2>&1 || { tail -30 "$BUILD/install.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/bin"
for p in as ld ar ranlib nm objdump objcopy strip readelf size strings \
         addr2line c++filt elfedit gprof ld.bfd; do
    [ -f "$STAGE/usr/bin/$p" ] && cp "$STAGE/usr/bin/$p" "$OUT/bin/$p"
done

echo "binutils $VERSION (native) -> $OUT"
ls -l "$OUT/bin" | awk 'NR>1 {print "   ", $9, $5}'
