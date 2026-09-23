#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GCC, to RUN ON the machine (task 49).
#
# The point of the exercise: a compiler on the Sage040 that can build
# the Sage040's own kernel, with no other computer involved.
#
# A CANADIAN CROSS, built on mother. --build is this workstation,
# --host is the machine (where gcc will run), --target is the machine
# (what it compiles for). host == target is what makes it native
# rather than a cross compiler that happens to run there.
#
# NOTHING IS COMPILED INSIDE THE EMULATOR. The emulator runs the
# result; that is the test, not the build.
#
# THE SAME VERSION AS THE CROSS COMPILER, 15.2.0, from the same source
# tree. A native compiler a version adrift from the cross one would
# produce subtly different code for the same source, which is exactly
# the kind of difference that is invisible until it matters.
#
# WHAT IS ON:
#   c, c++   C is the point. C++ is here because gdb is written in it
#            and will not build without a C++ runtime for this target
#            -- and because a system that can only build C is a system
#            half the world cannot be ported to.
#
# WHAT IS OFF, each because of something this machine is:
#   --disable-shared        no shared libgcc/libstdc++: ld.so works,
#                           but a shared libgcc means every program
#                           depends on one more file being right, for
#                           a library that is 500 KB
#   --disable-nls           gettext is not built for this machine
#   --without-headers=no    it HAS headers: picolibc's, at /usr/include
#   --with-newlib           picolibc is newlib's descendant, and this
#                           is what tells libstdc++ not to assume
#                           glibc's extensions
#   --disable-libssp        stack protector wants __stack_chk_guard
#                           from the C library; picolibc has none
#   --disable-libsanitizer  wants Linux's /proc and mmap semantics
#   --disable-libgomp       OpenMP, on a single-core machine
#   --disable-libvtv        vtable verification, needs its own runtime
#   --disable-bootstrap     a Canadian cross cannot bootstrap: the
#                           stage-2 compiler would have to RUN here
#   --enable-threads=posix  this system has pthreads (task 31a), and
#                           libstdc++ without threads is a libstdc++
#                           that cannot do std::thread or std::mutex
#
# gmp, mpfr and mpc come from their own ports rather than gcc's
# in-tree copies, so that the arithmetic libraries are built with the
# answers this target needs (see ports/gmp/build.sh) and so CLISP can
# share the same libgmp.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=15.2.0
SRC=$SRCDIR/gcc-$VERSION
BUILD=$SRCDIR/build-gcc-native
STAGE=$BUILD/stage
GMPOUT=$SRCDIR/build-gmp-sage040/sage040
MPFROUT=$SRCDIR/build-mpfr-sage040/sage040
MPCOUT=$SRCDIR/build-mpc-sage040/sage040
BINOUT=$SRCDIR/build-binutils-native/sage040

[ -d "$SRC" ] || {
    echo "gcc: $SRC is not there. The cross compiler was built from it" >&2
    echo "(toolchain.md); the native one must be the same version."     >&2
    exit 1
}
for d in "$GMPOUT/lib/libgmp.a" "$MPFROUT/lib/libmpfr.a" "$MPCOUT/lib/libmpc.a"; do
    [ -f "$d" ] || { echo "gcc: missing $d -- build ports/gmp, mpfr, mpc" >&2; exit 1; }
done
[ -x "$BINOUT/bin/as" ] || "$HERE/../binutils/build.sh"

HOST_TRIPLET=m68k-unknown-elf
BUILD_TRIPLET=$("$SRC/config.guess")

# See ports/binutils/build.sh: gcc has subdirectory configures too, and
# this system has no thread-local storage for them to find.
export ac_cv_tls=none

applied=$SRC/.sage040-patches
touch "$applied"
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    name=$(basename "$p")
    grep -qxF "$name" "$applied" && continue
    patch -d "$SRC" -p1 -N -s < "$p"
    echo "$name" >> "$applied"
done

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --build="$BUILD_TRIPLET" \
        --host="$HOST_TRIPLET" \
        --target="$HOST_TRIPLET" \
        --prefix=/usr \
        --program-prefix= \
        --enable-languages=c,c++ \
        --with-newlib \
        --with-gmp="$GMPOUT" \
        --with-mpfr="$MPFROUT" \
        --with-mpc="$MPCOUT" \
        --with-gnu-as --with-gnu-ld \
        --disable-nls \
        --disable-shared \
        --disable-bootstrap \
        --disable-libssp \
        --disable-libsanitizer \
        --disable-libgomp \
        --disable-libvtv \
        --disable-libquadmath \
        --disable-lto \
        --disable-plugin \
        --disable-multilib \
        --enable-threads=posix \
        --with-cpu=68040 \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        CXX="$CROSS_BIN/m68k-elf-g++ $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        CC_FOR_BUILD=cc \
        CXX_FOR_BUILD=c++ \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -40 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

rm -rf "$STAGE"
make -C "$BUILD" install DESTDIR="$STAGE" > "$BUILD/install.log" 2>&1 \
    || { tail -30 "$BUILD/install.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT"
cp -a "$STAGE/usr/." "$OUT/"

echo "gcc $VERSION (native) -> $OUT"
du -sh "$OUT" 2>/dev/null | awk '{print "   total", $1}'
