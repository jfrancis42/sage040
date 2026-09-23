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
CXXLIB=$SRCDIR/build-libstdcxx-sage040/sage040
CXXPREFIX=${SAGE_CXX:-$HOME/m68k/install-cxx}

[ -d "$SRC" ] || {
    echo "gcc: $SRC is not there. The cross compiler was built from it" >&2
    echo "(toolchain.md); the native one must be the same version."     >&2
    exit 1
}
for d in "$GMPOUT/lib/libgmp.a" "$MPFROUT/lib/libmpfr.a" "$MPCOUT/lib/libmpc.a"; do
    [ -f "$d" ] || { echo "gcc: missing $d -- build ports/gmp, mpfr, mpc" >&2; exit 1; }
done
[ -x "$BINOUT/bin/as" ] || "$HERE/../binutils/build.sh"
[ -f "$CXXLIB/lib/libstdc++.a" ] || "$HERE/../libstdcxx/build.sh"
[ -x "$CXXPREFIX/bin/m68k-elf-g++" ] || {
    echo "gcc: no cross g++ in $CXXPREFIX -- see progress.md, task 49." >&2
    exit 1
}

# The compiler that builds this one is the C++-capable cross gcc, and
# its own internal headers and its assembler have to be its own. See
# ports/libstdcxx/build.sh, where the same three lines are explained
# at length: the wrong gcc's include directory and the host's `as`
# both produce failures that name neither.
CXXCC=$CXXPREFIX/bin/m68k-elf-gcc
CXXCXX=$CXXPREFIX/bin/m68k-elf-g++
CXX_CPPFLAGS="-nostdinc -isystem $SAGE_LIBC/include \
-isystem $("$CXXCC" -print-file-name=include) -D_GNU_SOURCE \
-I$CXXLIB/include/c++/15.2.0 \
-I$CXXLIB/include/c++/15.2.0/m68k-unknown-elf"
CXX_BINDIR=$(dirname "$CROSS_CC")/../m68k-elf/bin
CXX_LIBGCC=$(dirname "$("$CROSS_CC" -mcpu=68040 -print-libgcc-file-name)")
CXX_TOOLS="-B$CXX_BINDIR/ -B$CXX_LIBGCC/ -L$CXX_LIBGCC -L$CXXLIB/lib"

HOST_TRIPLET=m68k-unknown-elf
BUILD_TRIPLET=$("$SRC/config.guess")

# See ports/binutils/build.sh: gcc has subdirectory configures too, and
# this system has no thread-local storage for them to find.
export ac_cv_tls=none

# gcc 15 carries gettext in its tree and CONFIGURES IT WHATEVER
# --disable-nls says -- there is no option that skips the directory.
# Its gnulib decides uselocale() is usable from a compile test, then
# calls it as `uselocale(NULL)`; picolibc's locale_t is not a pointer,
# so that is "makes integer from pointer without a cast" and the whole
# build stops in a library nothing here wants.
#
# Saying so through the cache variable is the truthful answer rather
# than a workaround: this C library's uselocale does not do what
# gnulib means by a working one.
export gt_cv_func_uselocale_works=no

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
mkdir -p "$BUILD"

# TOOLS UNDER THE TARGET'S OWN NAME.
#
# --target is m68k-unknown-elf, so gcc's build looks for
# m68k-unknown-elf-gcc, -as, -ld and the rest when it needs to compile
# something FOR the target -- and the cross toolchain in ~/m68k/install
# is installed as m68k-elf-*. Same compiler, same target, different
# spelling of the triplet.
#
# Without them the build got as far as linking xgcc and then ran
# `m68k-unknown-elf-gcc -dumpspecs`, which is not on PATH: "Error 127",
# command not found, in the middle of a compiler build.
#
# Note that these are the CROSS tools (build -> target), which is what
# gcc wants here. They are not the native ones being built; those
# cannot run on this workstation at all.
mkdir -p "$BUILD/toolbin"
#
# THE COMPILER ONES COME FROM install-cxx, the binutils ones from the
# ordinary cross prefix. gcc's build runs the target compiler on its own
# C++ self-test, and the C-only cross gcc answers "language c++ not
# recognized" -- which is true of it and is not what the build is
# asking about.
for t in gcc g++ c++ cpp; do
    [ -x "$CXXPREFIX/bin/m68k-elf-$t" ] && \
        ln -sf "$CXXPREFIX/bin/m68k-elf-$t" "$BUILD/toolbin/$HOST_TRIPLET-$t"
done
for t in as ld ar ranlib nm objdump objcopy strip readelf; do
    src=""
    if [ -x "$CROSS_BIN/m68k-elf-$t" ]; then
        src=$CROSS_BIN/m68k-elf-$t
    elif [ -x "$CXX_BINDIR/$t" ]; then
        src=$CXX_BINDIR/$t
    fi
    [ -n "$src" ] && ln -sf "$src" "$BUILD/toolbin/$HOST_TRIPLET-$t"
done
PATH=$BUILD/toolbin:$PATH
export PATH

if [ ! -f "$BUILD/Makefile" ]; then
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
        --without-isl \
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
        CC="$CXXCC $CXX_TOOLS $CROSS_CFLAGS $CXX_CPPFLAGS $SPECS_CFLAGS" \
        CXX="$CXXCXX $CXX_TOOLS $CROSS_CFLAGS $CXX_CPPFLAGS $SPECS_CFLAGS -lstdc++" \
        CC_FOR_BUILD=cc \
        CXX_FOR_BUILD=c++ \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -40 "$BUILD/configure.log"; exit 1; }
fi

# all-host AND install-host, NOT all and install.
#
# `all` would go on to build the TARGET libraries -- libgcc first --
# and building libgcc means RUNNING the compiler that was just built,
# which is an m68k program and cannot run on this workstation. The
# failure is configure-target-libgcc saying "cannot compute suffix of
# object files".
#
# There is nothing to build anyway: libgcc for this target already
# exists, from the cross toolchain, and is the same library from the
# same source at the same version. The C library is picolibc, built by
# libc/build.sh. What is missing here is only the compiler, and
# all-host is exactly the compiler.
make -C "$BUILD" -j"$(nproc)" all-host > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

rm -rf "$STAGE"
make -C "$BUILD" install-host DESTDIR="$STAGE" > "$BUILD/install.log" 2>&1 \
    || { tail -30 "$BUILD/install.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT"
cp -a "$STAGE/usr/." "$OUT/"

# STRIPPED, and it is not optional. gcc built with -g is 1.1 GB, which
# does not go on a 512 MB disk; cc1 alone is most of it. The debug
# information is of no use on the machine anyway -- there is no native
# gdb yet, and when there is, this is not the program anybody wants to
# debug with it. What is left is around a tenth of the size.
find "$OUT" -type f -perm -u+x | while read -r f; do
    case "$(head -c 4 "$f" | od -An -tx1 | tr -d " ")" in
        7f454c46) "$CROSS_BIN/m68k-elf-strip" --strip-unneeded "$f" \
                      2>/dev/null || true ;;
    esac
done

echo "gcc $VERSION (native) -> $OUT"
du -sh "$OUT" 2>/dev/null | awk '{print "   total", $1}'
