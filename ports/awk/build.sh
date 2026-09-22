#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - the one true awk (Kernighan's) for Sage040.
#
# Fetched as Debian's copy of the upstream snapshot, pinned by checksum:
# the upstream repository (github.com/onetrue-awk/awk) no longer
# answers, and Debian's pool keeps every release it shipped. Its
# licence is Lucent's permissive one, not the GPL, so the source is
# built from where it is and never copied into this tree.
#
# Three steps run on the HOST: bison makes the parser, and maketab -- a
# host program -- reads the parser's token numbers and writes proctab.c.
# Everything else is cross-compiled against picolibc. `make libc` first.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
TOP=$(cd ../.. && pwd)

VERSION=2025-12-25
SHA256=5eb8bb449848b2860ecff1fba09a0429853f849277c7e8fa2825ba4a553af920
URL=https://deb.debian.org/debian/pool/main/o/original-awk/original-awk_$VERSION.orig.tar.gz

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRC=$SRCDIR/original-awk-$VERSION

CC=$M68K_PREFIX/bin/m68k-elf-gcc
[ -x "$CC" ] || CC=$(command -v m68k-elf-gcc)
[ -f "$SAGE_LIBC/lib/libc.a" ] || {
    echo "build.sh: no picolibc in $SAGE_LIBC -- run 'make libc'" >&2
    exit 1
}
command -v bison >/dev/null || { echo "build.sh: needs bison" >&2; exit 1; }

if [ ! -d "$SRC" ]; then
    tarball=$SRCDIR/original-awk_$VERSION.orig.tar.gz
    mkdir -p "$SRCDIR"
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# On the host.
( cd "$SRC"
  [ awkgram.tab.c -nt awkgram.y ] || bison -d awkgram.y
  [ maketab -nt maketab.c ] || cc -O2 maketab.c -o maketab
  [ proctab.c -nt awkgram.tab.h ] || ./maketab awkgram.tab.h > proctab.c )

# For the machine.
CFLAGS="-mcpu=68040 -O2 -nostdinc -nostdlib \
        -isystem $SAGE_LIBC/include -isystem $("$CC" -print-file-name=include) \
        -ffunction-sections -fdata-sections -D_GNU_SOURCE"
mkdir -p "$SRC/sage040"
objs=
for f in b main parse proctab tran lib run lex awkgram.tab; do
    o="$SRC/sage040/$f.o"
    if [ ! -f "$o" ] || [ "$SRC/$f.c" -nt "$o" ]; then
        # shellcheck disable=SC2086
        "$CC" $CFLAGS -c "$SRC/$f.c" -o "$o"
    fi
    objs="$objs $o"
done
# Against /lib/libc.so, as every ported program is by default; LINK=static
# for one that needs no /lib (see ports/uemacs/build.sh).
# shellcheck disable=SC2086
if [ "${LINK:-dynamic}" = static ]; then
    "$CC" -mcpu=68040 -nostdlib -Wl,-Bstatic -T "$TOP/libc/sage040.ld" \
        -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,--gc-sections \
        "$TOP/libc/crt0.s" $objs -L"$SAGE_LIBC/lib" \
        -Wl,--start-group -lc -llinux -lm -Wl,--end-group -lgcc \
        -o "$HERE/awk"
else
    "$CC" -mcpu=68040 -nostdlib -Wl,-Ttext-segment=0x10000000 \
        -Wl,--dynamic-linker=/lib/ld.so -Wl,-z,now -Wl,--hash-style=sysv \
        -Wl,--build-id=none -Wl,--gc-sections \
        "$TOP/libc/crt0-dyn.s" $objs -L"$SAGE_LIBC/lib" \
        -lc -lgcc -o "$HERE/awk"
fi
"${CC%gcc}size" "$HERE/awk"
