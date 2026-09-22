#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - vi for Sage040: Ali Gholami Rudi's neatvi.
#
# A small, complete vi and ex, ISC-licensed, that talks to the terminal
# with escape sequences of its own and needs nothing but POSIX. Fetched
# at a fixed commit, the way ports/uemacs is, with patches/ applied, and
# built against picolibc -- `make libc` first.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
TOP=$(cd ../.. && pwd)

URL=https://github.com/aligrudi/neatvi.git
COMMIT=930f42a75a664c33cf55eacd947ca0722dd6d509

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRC=$SRCDIR/neatvi-${COMMIT:0:7}

CC=$M68K_PREFIX/bin/m68k-elf-gcc
[ -x "$CC" ] || CC=$(command -v m68k-elf-gcc)
[ -f "$SAGE_LIBC/lib/liblinux.a" ] || {
    echo "build.sh: no picolibc in $SAGE_LIBC -- run 'make libc'" >&2
    exit 1
}

if [ ! -d "$SRC" ]; then
    git clone -q "$URL" "$SRC"
    git -C "$SRC" checkout -q "$COMMIT"
    for p in "$HERE"/patches/*.patch; do
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# The objects neatvi's own Makefile links into vi.
OBJS="vi ex lbuf mot sbuf ren dir syn reg led uc term rset rstr regex cmd
      tag conf lsp json"
CFLAGS="-mcpu=68040 -O2 -Wall -Wno-format-truncation -nostdinc -nostdlib \
        -isystem $SAGE_LIBC/include -isystem $("$CC" -print-file-name=include) \
        -ffunction-sections -fdata-sections -D_GNU_SOURCE"
mkdir -p "$SRC/sage040"
objs=
for f in $OBJS; do
    o="$SRC/sage040/$f.o"
    if [ ! -f "$o" ] || [ "$SRC/$f.c" -nt "$o" ]; then
        # shellcheck disable=SC2086
        "$CC" $CFLAGS -c "$SRC/$f.c" -o "$o"
    fi
    objs="$objs $o"
done
# shellcheck disable=SC2086
"$CC" -mcpu=68040 -nostdlib -T "$TOP/libc/sage040.ld" \
    -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,--gc-sections \
    "$TOP/libc/crt0.s" $objs -L"$SAGE_LIBC/lib" \
    -Wl,--start-group -lc -llinux -Wl,--end-group -lgcc \
    -o "$HERE/vi"
"${CC%gcc}size" "$HERE/vi"
