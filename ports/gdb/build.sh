#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GDB, to RUN ON the machine: gdb itself, and gdbserver.
#
# The same version as the cross gdb (17.1), from a tree of its own:
# the cross gdb's source in ~/m68k/src/gdb-17.1 is left exactly as it
# was, because the patches here are about the machine gdb runs ON.
#
# --host IS m68k-unknown-linux-gnu, not the m68k-unknown-elf every
# other port says. gdb chooses its native support from the host
# triplet (configure.host, configure.nat): an elf host has no native
# target at all, only remote, and a linux one gets linux-nat -- ptrace,
# waitpid, /proc -- which is what the kernel now speaks. The compiler
# is still the m68k-elf one with picolibc; the tools are linked under
# the linux name in toolbin/.
#
# C++, so it is built with the second cross compiler and libstdc++,
# exactly as ports/gcc is.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=17.1
SHA256=14996f5f74c9f68f5a543fdc45bca7800207f91f92aeea6c2e791822c7c6d876
URL=https://ftp.gnu.org/gnu/gdb/gdb-$VERSION.tar.xz
TARBALL=$SRCDIR/gdb-$VERSION.tar.xz
SRC=$SRCDIR/gdb-$VERSION-sage040
BUILD=$SRCDIR/build-gdb-sage040
OUT=$BUILD/sage040

GMPOUT=$SRCDIR/build-gmp-sage040/sage040
MPFROUT=$SRCDIR/build-mpfr-sage040/sage040
NCOUT=$SRCDIR/build-ncurses-sage040/sage040
ICONVOUT=$SRCDIR/build-libiconv-sage040/sage040
CXXLIB=$SRCDIR/build-libstdcxx-sage040/sage040
CXXPREFIX=${SAGE_CXX:-$HOME/m68k/install-cxx}

[ -f "$GMPOUT/lib/libgmp.a" ] || "$HERE/../gmp/build.sh"
[ -f "$MPFROUT/lib/libmpfr.a" ] || "$HERE/../mpfr/build.sh"
[ -f "$NCOUT/lib/libncurses.a" ] || "$HERE/../ncurses/build.sh"
[ -f "$ICONVOUT/lib/libiconv.a" ] || "$HERE/../libiconv/build.sh"
[ -f "$CXXLIB/lib/libstdc++.a" ] || "$HERE/../libstdcxx/build.sh"

if [ ! -d "$SRC" ]; then
    [ -f "$TARBALL" ] || curl -L --fail -o "$TARBALL" "$URL"
    echo "$SHA256  $TARBALL" | sha256sum -c - >/dev/null
    rm -rf "$SRC.tmp"; mkdir -p "$SRC.tmp"
    tar -C "$SRC.tmp" -xf "$TARBALL"
    mv "$SRC.tmp/gdb-$VERSION" "$SRC"; rmdir "$SRC.tmp"
fi

applied=$SRC/.sage040-patches
touch "$applied"
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    name=$(basename "$p")
    grep -qxF "$name" "$applied" && continue
    patch -d "$SRC" -p1 -N -s < "$p"
    echo "$name" >> "$applied"
done

# See ports/gcc/build.sh for each of these lines.
CXXCC=$CXXPREFIX/bin/m68k-elf-gcc
CXXCXX=$CXXPREFIX/bin/m68k-elf-g++
CXX_CPPFLAGS="-nostdinc -isystem $SAGE_LIBC/include \
-isystem $("$CXXCC" -print-file-name=include) -D_GNU_SOURCE \
-I$CXXLIB/include/c++/15.2.0 \
-I$CXXLIB/include/c++/15.2.0/m68k-unknown-elf"
CXX_BINDIR=$(dirname "$CROSS_CC")/../m68k-elf/bin
CXX_LIBGCC=$(dirname "$("$CROSS_CC" -mcpu=68040 -print-libgcc-file-name)")
CXX_TOOLS="-B$CXX_BINDIR/ -B$CXX_LIBGCC/ -L$CXX_LIBGCC -L$CXXLIB/lib"

GDB_TRIPLET=m68k-unknown-linux-gnu
BUILD_TRIPLET=$("$SRC/config.guess")

libc_fresh "$BUILD" || true
mkdir -p "$BUILD/toolbin"
for t in gcc g++ c++ cpp; do
    ln -sf "$CXXPREFIX/bin/m68k-elf-$t" "$BUILD/toolbin/$GDB_TRIPLET-$t"
done
for t in as ld ar ranlib nm objdump objcopy strip readelf; do
    ln -sf "$CROSS_BIN/m68k-elf-$t" "$BUILD/toolbin/$GDB_TRIPLET-$t"
done
PATH=$BUILD/toolbin:$PATH
export PATH

# -fcommon, for C only: readline's terminal.c says `char PC, *BC, *UP;`
# -- tentative definitions, which it expects to merge with the termcap
# library's. gcc 15 defaults to -fno-common, which makes them real
# definitions, and the link fails on three "multiple definition"s
# against libc's libtermcap.

# The answers configure cannot find out by running something here.
export ac_cv_tls=none
export gt_cv_func_uselocale_works=no
export gl_cv_func_getopt_posix=no

if [ ! -f "$BUILD/Makefile" ]; then
    (cd "$BUILD" && "$SRC/configure" \
        --build="$BUILD_TRIPLET" \
        --host="$GDB_TRIPLET" \
        --target="$GDB_TRIPLET" \
        --prefix=/usr \
        --disable-nls \
        --disable-sim \
        --disable-werror \
        --disable-inprocess-agent \
        --disable-source-highlight \
        --disable-libbacktrace \
        --disable-unit-tests \
        --enable-gdbserver=yes \
        --with-gmp="$GMPOUT" \
        --with-mpfr="$MPFROUT" \
        --with-libiconv-prefix="$ICONVOUT" --with-libiconv-type=static \
        --without-python --without-guile --without-expat \
        --without-lzma --without-zstd --without-debuginfod \
        --without-babeltrace --without-intel-pt --without-xxhash \
        --without-libipt --with-system-zlib=no \
        CC="$CXXCC $CXX_TOOLS $CROSS_CFLAGS -fcommon $CXX_CPPFLAGS $SPECS_CFLAGS" \
        CXX="$CXXCXX $CXX_TOOLS $CROSS_CFLAGS $CXX_CPPFLAGS $SPECS_CFLAGS" \
        CPPFLAGS="-I$ICONVOUT/include -I$NCOUT/include -I$NCOUT/include/ncurses" \
        LDFLAGS="-L$NCOUT/lib" \
        CC_FOR_BUILD=cc CXX_FOR_BUILD=c++ \
        AR="$CROSS_BIN/m68k-elf-ar" RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -40 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" all-gdb all-gdbserver > "$BUILD/make.log" 2>&1 \
    || { grep -E 'error|Error' "$BUILD/make.log" | head -40; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$BUILD/gdb/gdb" "$BUILD/gdbserver/gdbserver" "$OUT/bin/"
"$CROSS_BIN/m68k-elf-strip" --strip-unneeded "$OUT/bin/gdb" "$OUT/bin/gdbserver"
echo "gdb $VERSION (native) -> $OUT"
ls -l "$OUT/bin"
