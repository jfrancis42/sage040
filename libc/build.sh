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
#   mb-capable            multibyte locales, so a program can ask for
#                         C.UTF-8 (LANG, setlocale) -- names here are
#                         UTF-8 bytes. The default is still "C", as on
#                         Linux with no LANG set.
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
        -Dmb-capable=true \
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

# The network layer (libc/net): the socket calls, inet_*, and a resolver
# with getaddrinfo -- picolibc has none of it. Headers first, over
# picolibc's (its arpa/inet.h has the byte-order macros and nothing
# else; ours has those and the rest), then into libc.a.
cp -r "$HERE/net/include/." "$PREFIX/include/"
# And the headers the m68k backend adds that meson does not know to
# install (the overlay puts them where the build finds them).
cp "$HERE/picolibc/libc/include/sys/utsname.h" "$PREFIX/include/sys/"
cp "$HERE/picolibc/libc/include/stdio_ext.h" "$PREFIX/include/"
NET_OBJS=
for src in "$HERE"/net/src/*.c; do
    obj="$BUILD/net-$(basename "$src" .c).o"
    "$CC" -mcpu=68040 -O2 -Wall -Wextra -nostdinc -nostdlib \
        -isystem "$PREFIX/include" -isystem "$("$CC" -print-file-name=include)" \
        -c "$src" -o "$obj"
    NET_OBJS="$NET_OBJS $obj"
done
# shellcheck disable=SC2086
"$BIN/m68k-elf-ar" rcs "$PREFIX/lib/libc.a" $NET_OBJS

# libc.so: the same sources again, position independent, in a build of
# their own (a PIC object is slower -- a5 holds the GOT -- so the static
# libc.a is not built this way). Nothing from this build is installed
# except the one shared object linked from its archives.
#
# -fPIC: the whole library is reached through its GOT, so nothing in
# its text needs relocating and every process shares the same pages.
# libgcc is not PIC and there is no PIC build of it, so its code is
# renamed .libgcc and marked writable, and libc-so.ld puts it in the
# data segment: its absolute addresses become data relocations, in a
# few pages each process copies anyway. `readelf -d` showing no TEXTREL
# is the check that the text is shareable.
BUILD_PIC=$BUILD-pic
if [ ! -f "$BUILD_PIC/build.ninja" ]; then
    meson setup "$BUILD_PIC" "$SRC" \
        --cross-file "$BUILD/cross-sage040.txt" \
        --prefix="$BUILD_PIC/staging" \
        -Dc_args=-fPIC \
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
        -Dmb-capable=true \
        -Dstdio-exit-flush=true \
        -Dfstat-bufsiz=true \
        -Dspecsdir=none \
        -Dincludedir=include \
        -Dlibdir=lib
fi
ninja -C "$BUILD_PIC"
# The archive, less the one member that is not for a hosted system:
# interrupt.c.o, picolibc's bare-metal m68k vector table, which wants
# __stack and _start. A static link never pulls it in; linking the
# whole archive does.
cp "$BUILD_PIC/libc.a" "$BUILD_PIC/libc-so.a"
"$BIN/m68k-elf-ar" d "$BUILD_PIC/libc-so.a" interrupt.c.o
# And the network layer, position independent, into the same archive.
for src in "$HERE"/net/src/*.c; do
    obj="$BUILD_PIC/net-$(basename "$src" .c).o"
    "$CC" -mcpu=68040 -O2 -fPIC -Wall -Wextra -nostdinc -nostdlib \
        -isystem "$PREFIX/include" -isystem "$("$CC" -print-file-name=include)" \
        -c "$src" -o "$obj"
    "$BIN/m68k-elf-ar" rcs "$BUILD_PIC/libc-so.a" "$obj"
done
"$BIN/m68k-elf-objcopy" --rename-section .text=.libgcc,alloc,load,contents,code \
    "$("$CC" -mcpu=68040 -print-libgcc-file-name)" "$BUILD_PIC/libgcc-rw.a"
"$BIN/m68k-elf-ld" -shared -soname libc.so -T "$HERE/libc-so.ld" \
    --hash-style=sysv -z now --build-id=none --no-warn-rwx-segments \
    -o "$BUILD_PIC/libc.so" \
    --whole-archive "$BUILD_PIC/libc-so.a" \
    "$BUILD_PIC/libos/linux/liblinux.a" --no-whole-archive \
    "$BUILD_PIC/libos/fallback/libos-fallback.a" \
    "$BUILD_PIC/libgcc-rw.a"
if "$BIN/m68k-elf-readelf" -d "$BUILD_PIC/libc.so" | grep -q TEXTREL; then
    echo "build.sh: libc.so has text relocations" >&2
    exit 1
fi
# Nothing left undefined but the weak hooks a program may supply: an
# undefined symbol here is a failure at every program's start.
undef=$("$BIN/m68k-elf-nm" -D --undefined-only "$BUILD_PIC/libc.so" | awk '$1 != "w"')
if [ -n "$undef" ]; then
    echo "build.sh: libc.so leaves these undefined:" >&2
    echo "$undef" >&2
    exit 1
fi
install -m 644 "$BUILD_PIC/libc.so" "$PREFIX/lib/libc.so"

echo "picolibc $VERSION installed in $PREFIX"
