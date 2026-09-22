#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - build picolibc for Sage040 programs.
#
# picolibc already knows how to run on Linux: libos/linux is a POSIX
# layer over Linux's system calls, translating its own errno and signal
# numbers to and from Linux's. It has backends for arm, aarch64 and x86
# and not for m68k. This kernel's system call interface IS Linux/m68k's,
# numbers and structures, on purpose -- so the port is the m68k backend
# in picolibc/, laid over the release, plus the fixes in patches/ for
# bugs in picolibc that are not about m68k.
#
# The result is installed outside the tree, the way the toolchain and
# the emulator are: $SAGE_LIBC, by default ~/m68k/sage040-libc.
#
#   ./build.sh          fetch if needed, build, install
#   ./build.sh clean    remove the build directory

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)

VERSION=1.8.12
SHA256=64e8c412e1c40fa6eb1a72d2b5cdbcbfe6ceca4cbea454edbad54557ffc747fa
URL=https://github.com/picolibc/picolibc/releases/download/$VERSION/picolibc-$VERSION.tar.xz

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
PREFIX=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
BUILD=$SRCDIR/build-picolibc-sage040
SRC=$SRCDIR/picolibc-$VERSION

if [ "${1:-}" = clean ]; then
    rm -rf "$BUILD"
    exit 0
fi

CC=$M68K_PREFIX/bin/m68k-elf-gcc
[ -x "$CC" ] || CC=$(command -v m68k-elf-gcc) || {
    echo "build.sh: no m68k-elf-gcc; see toolchain.md" >&2
    exit 1
}
BIN=$(dirname "$CC")

mkdir -p "$SRCDIR"
if [ ! -d "$SRC" ]; then
    tarball=$SRCDIR/picolibc-$VERSION.tar.xz
    if [ ! -f "$tarball" ]; then
        curl -L --fail -o "$tarball" "$URL"
    fi
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

# The m68k backend: new files only.
cp -r "$HERE"/picolibc/. "$SRC"/

# And fixes to picolibc itself, each a bug found here that is not about
# m68k -- see the head of each patch. Applied once: a patch that already
# reverses cleanly is in.
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    if patch -d "$SRC" -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
        continue
    fi
    patch -d "$SRC" -p1 -N -s < "$p"
done

# The cross file, with this machine's compiler in it.
mkdir -p "$BUILD"
cat > "$BUILD/cross-sage040.txt" <<EOF
[binaries]
c = ['$BIN/m68k-elf-gcc', '-mcpu=68040', '-nostdlib']
ar = '$BIN/m68k-elf-ar'
as = '$BIN/m68k-elf-as'
ld = '$BIN/m68k-elf-ld'
nm = '$BIN/m68k-elf-nm'
strip = '$BIN/m68k-elf-strip'

[host_machine]
system = 'linux'
cpu_family = 'm68k'
cpu = '68040'
endian = 'big'

[properties]
skip_sanity_check = true
EOF

# What each option is for:
#   os-linux              the Linux system call layer: the point
#   semihost, picocrt     off: there is an operating system here, and
#                         programs start through libc/crt0.s
#   multilib              off: one CPU, the 68040 with its FPU
#   thread-local-storage  off: a process has one thread, so errno is a
#                         plain global and there is no TLS to set up
#   single-thread         no locking, for the same reason
#   io-long-long          %lld in printf, which real programs use
#   stdio-exit-flush      stdout flushed at exit, as everyone expects
#   fstat-bufsiz          stdio buffers sized from st_blksize
if [ ! -f "$BUILD/build.ninja" ]; then
    meson setup "$BUILD" "$SRC" \
        --cross-file "$BUILD/cross-sage040.txt" \
        --prefix="$PREFIX" \
        -Dbuildtype=release \
        -Doptimization=2 \
        -Dos-linux=true \
        -Dsemihost=false \
        -Dfake-semihost=false \
        -Dpicocrt=false \
        -Dpicocrt-lib=false \
        -Dmultilib=false \
        -Dtests=false \
        -Dthread-local-storage=false \
        -Dsingle-thread=true \
        -Dio-long-long=true \
        -Dstdio-exit-flush=true \
        -Dfstat-bufsiz=true \
        -Dspecsdir=none \
        -Dincludedir=include \
        -Dlibdir=lib
fi
ninja -C "$BUILD"
ninja -C "$BUILD" install

# termcap, with the one terminal compiled in (termcap/termcap.c), built
# against what was just installed.
"$CC" -mcpu=68040 -O2 -Wall -Wextra -nostdinc -nostdlib \
    -isystem "$PREFIX/include" -isystem "$("$CC" -print-file-name=include)" \
    -c "$HERE/termcap/termcap.c" -o "$BUILD/termcap.o"
rm -f "$PREFIX/lib/libtermcap.a"
"$BIN/m68k-elf-ar" rcs "$PREFIX/lib/libtermcap.a" "$BUILD/termcap.o"
cp "$HERE/termcap/termcap.h" "$PREFIX/include/termcap.h"

echo "picolibc $VERSION installed in $PREFIX"
