#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - uEmacs/PK for Sage040.
#
# The source is Linus Torvalds' uEmacs/PK, fetched at a fixed commit and
# never copied into this tree: its licence (MicroEMACS 3.9's, free for
# non-commercial use) is not the GPL, so it is built from where it is,
# with the changes in patches/ applied. The result, ./em, is built
# against picolibc and libc/termcap -- `make libc` first.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
TOP=$(cd ../.. && pwd)

URL=https://github.com/torvalds/uemacs.git
COMMIT=325a8e97973729da7d288e6deb49b36afa0b252a

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRC=$SRCDIR/uemacs-${COMMIT:0:7}

CC=$M68K_PREFIX/bin/m68k-elf-gcc
[ -x "$CC" ] || CC=$(command -v m68k-elf-gcc)
[ -f "$SAGE_LIBC/lib/libtermcap.a" ] || {
    echo "build.sh: no picolibc/termcap in $SAGE_LIBC -- run 'make libc'" >&2
    exit 1
}

if [ ! -d "$SRC" ]; then
    git clone -q "$URL" "$SRC"
    git -C "$SRC" checkout -q "$COMMIT"
    for p in "$HERE"/patches/*.patch; do
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

CFLAGS="-mcpu=68040 -O2 -Wall -nostdinc -nostdlib \
        -isystem $SAGE_LIBC/include -isystem $("$CC" -print-file-name=include) \
        -ffunction-sections -fdata-sections -DPOSIX -D_GNU_SOURCE"
. "$HERE/../cross.sh"
libc_fresh "$SRC/sage040" || true   # rebuilt whole if picolibc's headers changed
objs=
for f in "$SRC"/*.c; do
    o="$SRC/sage040/$(basename "${f%.c}").o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
        # shellcheck disable=SC2086
        "$CC" $CFLAGS -c "$f" -o "$o"
    fi
    objs="$objs $o"
done
# Against /lib/libc.so by default, as libc/libc.mk links a program with
# LINK=dynamic; LINK=static for a program that needs no /lib. Either way
# the linker is told which it is: this gcc does not pass -static on, and
# with libc.so beside libc.a a bare -lc finds the shared one.
# shellcheck disable=SC2086
if [ "${LINK:-dynamic}" = static ]; then
    "$CC" -mcpu=68040 -nostdlib -Wl,-Bstatic -T "$TOP/libc/sage040.ld" \
        -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,--gc-sections \
        "$TOP/libc/crt0.s" $objs -L"$SAGE_LIBC/lib" \
        -Wl,--start-group -lc -llinux -ltermcap -Wl,--end-group -lgcc \
        -o "$HERE/em"
else
    "$CC" -mcpu=68040 -nostdlib -Wl,-Ttext-segment=0x10000000 \
        -Wl,--dynamic-linker=/lib/ld.so -Wl,-z,now -Wl,--hash-style=sysv \
        -Wl,--build-id=none -Wl,--gc-sections \
        "$TOP/libc/crt0-dyn.s" $objs -L"$SAGE_LIBC/lib" \
        -ltermcap -lc -lgcc -o "$HERE/em"
fi
"${CC%gcc}size" "$HERE/em"
