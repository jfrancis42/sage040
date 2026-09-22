# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# cross.sh - sourced by a port's build.sh: the variables a GNU-style
# `./configure --host=... && make` needs to build for this machine.
#
# configure's link tests are static links, which need nothing on the
# machine; the program itself is linked against /lib/libc.so by passing
# DYN_LDFLAGS/DYN_LIBS to make instead (`make LDFLAGS=... LIBS=...`).
#
# --host=m68k-unknown-elf, not m68k-linux-gnu: gnulib's cross-compiling
# guesses assume glibc for a *-linux-gnu host and skip replacing what
# glibc gets right and picolibc may not. An unknown host gets its
# conservative guesses, and replacements where it is unsure.

TOP=${TOP:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

CROSS_CC=$M68K_PREFIX/bin/m68k-elf-gcc
[ -x "$CROSS_CC" ] || CROSS_CC=$(command -v m68k-elf-gcc)
[ -f "$SAGE_LIBC/lib/libc.so" ] || {
    echo "cross.sh: no picolibc in $SAGE_LIBC -- run 'make libc'" >&2
    exit 1
}
CROSS_BIN=$(dirname "$CROSS_CC")

HOST_TRIPLET=m68k-unknown-elf
# The include paths in CPPFLAGS, not CFLAGS: configure's preprocessor-only
# tests (AC_EGREP_CPP, AC_PREPROC_IFELSE) use CPPFLAGS alone, and with the
# paths only in CFLAGS they read the compiler's bare headers instead of
# picolibc's -- gnulib then decided PATH_MAX did not exist, and more.
CROSS_CPPFLAGS="-nostdinc -isystem $SAGE_LIBC/include \
-isystem $("$CROSS_CC" -print-file-name=include) -D_GNU_SOURCE"
CROSS_CFLAGS="-mcpu=68040 -O2"
STATIC_LDFLAGS="-nostdlib -Wl,-Bstatic -T $TOP/libc/sage040.ld \
-Wl,--build-id=none -Wl,--no-warn-rwx-segments $TOP/libc/crt0.s \
-L$SAGE_LIBC/lib"
STATIC_LIBS="-Wl,--start-group -lc -llinux -Wl,--end-group -lgcc"
DYN_LDFLAGS="-nostdlib -Wl,-Ttext-segment=0x10000000 \
-Wl,--dynamic-linker=/lib/ld.so -Wl,-z,now -Wl,--hash-style=sysv \
-Wl,--build-id=none $TOP/libc/crt0-dyn.s -L$SAGE_LIBC/lib"
DYN_LIBS="-lc -lgcc"

# configure, cross: CC and friends, and the answers configure cannot
# find out without running a program on the machine.
cross_configure() {             # cross_configure SRCDIR [configure args...]
    local src=$1
    shift
    "$src/configure" --host="$HOST_TRIPLET" --build="$("$src/build-aux/config.guess")" \
        CC="$CROSS_CC" \
        AR="$CROSS_BIN/m68k-elf-ar" RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        CPPFLAGS="$CROSS_CPPFLAGS" CFLAGS="$CROSS_CFLAGS" \
        LDFLAGS="$STATIC_LDFLAGS" LIBS="$STATIC_LIBS" \
        "$@"
}
