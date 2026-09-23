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

# THE SPECS FILE, which is the other way to link for this machine.
#
# Everything above hands configure a link line spelled out in full.
# That works while a Makefile in this tree does the linking, and stops
# working the moment something else does: binutils' top-level configure
# checks the compiler with
#
#     ${CC} -o conftest ${CFLAGS} ${CPPFLAGS} ${LDFLAGS} conftest.c
#
# and no ${LIBS} anywhere, so a link line that depends on LIBS cannot
# pass it. libc/sage040.specs puts the knowledge in the compiler
# instead: `gcc hello.c -o hello` links a dynamic program, `gcc -static`
# a static one, and nothing else has to be said. It is also exactly what
# the NATIVE compiler needs, since somebody typing at a prompt on the
# machine will say no more than that either.
#
# -B, not -L, for the start files and the linker script: gcc looks for
# %s files in the startfile prefixes, and -L directories are not among
# them.
SAGE_SPECS=$TOP/libc/sage040.specs
SPECS_CFLAGS="-specs=$SAGE_SPECS -B$TOP/libc/ -B$SAGE_LIBC/lib/ -L$SAGE_LIBC/lib"

# The start files have to exist as OBJECTS for a spec to name one.
for _crt in crt0 crt0-dyn; do
    if [ ! -f "$TOP/libc/$_crt.o" ] ||        [ "$TOP/libc/$_crt.s" -nt "$TOP/libc/$_crt.o" ]; then
        "$CROSS_CC" -mcpu=68040 -c "$TOP/libc/$_crt.s" -o "$TOP/libc/$_crt.o"
    fi
done
unset _crt

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

# libc_fresh DIR: empty DIR if the C library's headers have changed since
# what is in it was built against them, and note the ones it is being
# built against now. Returns 1 if it emptied it. A header change can
# change a structure's size -- struct tm grew -- and an object built with
# the old one is silently wrong; nothing else here notices.
libc_fresh() {
    local sum
    sum=$(cat "$SAGE_LIBC/lib/.headers-sum" 2>/dev/null || echo none)
    if [ -d "$1" ] && [ "$(cat "$1/.libc-headers" 2>/dev/null)" = "$sum" ]; then
        return 0
    fi
    rm -rf "$1"
    mkdir -p "$1"
    echo "$sum" > "$1/.libc-headers"
    return 1
}

