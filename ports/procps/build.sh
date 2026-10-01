#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - procps-ng: ps, top, free, pgrep, pkill, pidof, pmap, vmstat,
# watch, uptime, w, kill, sysctl, slabtop, tload, pwdx. Not pidwait,
# which needs pidfd_open, which the kernel does not have.
#
# Everything here reads /proc, and reads it as it is on Linux -- the
# fifty-two fields of /proc/PID/stat, status, statm, /proc/meminfo,
# /proc/stat's btime -- which is what makes it the test of kernel/procfs.c
# that writing a ps of our own would not be. Against picolibc and the
# ncurses port (top and watch); libproc2 linked in statically.
#
# Nothing built lands in this directory: `make install` copies it onto
# the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=4.0.5
SHA256=c2e6d193cc78f84cd6ddb72aaf6d5c6a9162f0470e5992092057f5ff518562fa
URL=https://downloads.sourceforge.net/project/procps-ng/Production/procps-ng-$VERSION.tar.xz
SRC=$SRCDIR/procps-ng-$VERSION
BUILD=$SRCDIR/build-procps-sage040
OUT=$BUILD/sage040
NCOUT=$SRCDIR/build-ncurses-sage040/sage040

if [ ! -f "$SRC/.patched" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/procps-ng-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    rm -rf "$SRC"
    tar -C "$SRCDIR" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
    touch "$SRC/.patched"
fi

if [ ! -f "$NCOUT/lib/libncursesw.a" ]; then
    "$HERE/../ncurses/build.sh"
fi

libc_fresh "$BUILD" || true

# ncurses installs curses.h, and procps includes ncurses.h, as most
# programs do: the same header under its other name.
mkdir -p "$BUILD/compat"
[ -f "$BUILD/compat/ncurses.h" ] || echo '#include <curses.h>' > "$BUILD/compat/ncurses.h"

if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" --prefix=/usr \
        --disable-shared --enable-static --disable-nls \
        --without-systemd --without-elogind --disable-numa \
        --disable-harden-flags --enable-year2038 \
        --disable-pidwait \
        ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
        ac_cv_func_mmap_fixed_mapped=yes ac_cv_func_strcoll_works=yes \
        ac_cv_func_strtod=yes \
        NCURSES_CFLAGS="-I$NCOUT/include -I$BUILD/compat" \
        NCURSES_LIBS="-L$NCOUT/lib -lncursesw -ltinfow" \
        CPPFLAGS="$CROSS_CPPFLAGS -I$NCOUT/include -I$BUILD/compat" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

# The programs, by name: `make all` would build the test suite's helper
# programs too, and they want hugetlb and sched_setscheduler.
PROGS="src/ps/pscommand src/free src/pgrep src/pkill src/pmap src/pwdx
src/tload src/vmstat src/sysctl src/top/top src/watch src/kill src/pidof
src/slabtop src/w src/uptime"
make -C "$BUILD" -j"$(nproc)" \
    LDFLAGS="$DYN_LDFLAGS -L$NCOUT/lib" LIBS="$DYN_LIBS" $PROGS \
    > "$BUILD/make.log" 2>&1 || { grep -E 'error|Error' "$BUILD/make.log" | head -30; exit 1; }

mkdir -p "$OUT/bin"
for p in $PROGS; do
    n=$(basename "$p")
    [ "$n" = pscommand ] && n=ps
    cp "$BUILD/$p" "$OUT/bin/$n"
    "$CROSS_BIN/m68k-elf-strip" "$OUT/bin/$n"
done
echo "procps-ng $VERSION -> $OUT"
ls "$OUT/bin" | tr '\n' ' '; echo
