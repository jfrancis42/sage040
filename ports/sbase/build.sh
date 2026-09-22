#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - sbase, suckless's POSIX utilities, for SuckOS.
#
# Cloned at a fixed commit into ~/m68k/src and built from there; the
# source (MIT) is not copied into this tree. sbase's own Makefile builds
# its `make` first and then runs it, which cannot work when the result is
# for another machine, so this makes its two libraries with that Makefile
# and compiles and links each utility itself, against /lib/libc.so. The
# programs land in ./bin.
#
# Left out: grep and sed, which are GNU's here (ports/grep, ports/sed).

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

URL=https://git.suckless.org/sbase
COMMIT=c546c3a5724c81cee9a11d816a38ccdf17472129
SRC=$SRCDIR/sbase-${COMMIT:0:7}

if [ ! -d "$SRC" ]; then
    git clone -q "$URL" "$SRC"
    git -C "$SRC" checkout -q "$COMMIT"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# Everything rebuilt if picolibc's headers changed since the last build.
if ! libc_fresh "$HERE/bin"; then
    make -C "$SRC" clean >/dev/null 2>&1 || true
fi

# sbase's own feature macros, then this machine's headers.
SB_CPPFLAGS="-DPREFIX=\"\" -D_DEFAULT_SOURCE -D_NETBSD_SOURCE -D_BSD_SOURCE \
-D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 $CROSS_CPPFLAGS"
MK="make -C $SRC CC=$CROSS_CC AR=$CROSS_BIN/m68k-elf-ar RANLIB=$CROSS_BIN/m68k-elf-ranlib"
# shellcheck disable=SC2086
$MK -j8 CPPFLAGS="$SB_CPPFLAGS" CFLAGS="$CROSS_CFLAGS" libutf.a libutil.a >/dev/null

BIN=$(sed -n '/^BIN =/,/^$/p' "$SRC/Makefile" | tr -d '\\\t' | tr ' ' '\n' |
      grep -v '^BIN$\|^=$\|^$\|^grep$\|^sed$')
mkdir -p "$HERE/bin"
built=0
failed=
for p in $BIN; do
    name=$(basename "$p")
    out="$HERE/bin/$name"
    if [ -f "$out" ] && [ "$out" -nt "$SRC/$p.c" ] && [ "$out" -nt "$SRC/libutil.a" ]; then
        built=$((built + 1))
        continue
    fi
    case "$p" in
    make/make)
        srcs="$SRC"/make/*.c ;;
    bc)
        # A yacc grammar, turned into C on the host.
        (cd "$SRC" && [ bc.c -nt bc.y ] || yacc -o bc.c bc.y)
        srcs="$SRC/bc.c" ;;
    getconf)
        (cd "$SRC" && [ -f getconf.h ] || CC="$CROSS_CC $SB_CPPFLAGS" scripts/getconf.sh > getconf.h)
        srcs="$SRC/getconf.c" ;;
    *)
        srcs="$SRC/$p.c" ;;
    esac
    # shellcheck disable=SC2086
    if "$CROSS_CC" $SB_CPPFLAGS $CROSS_CFLAGS -I"$SRC" $DYN_LDFLAGS -o "$out" \
           $srcs "$SRC/libutil.a" "$SRC/libutf.a" $DYN_LIBS 2> "$HERE/bin/.$name.log"; then
        rm -f "$HERE/bin/.$name.log"
        built=$((built + 1))
    else
        rm -f "$out"
        failed="$failed $name"
    fi
done
# bc -l's library, where bc looks for it: PREFIX/share/misc.
mkdir -p "$HERE/share/misc"
cp "$SRC/bc.library" "$HERE/share/misc/bc.library"

echo "sbase: $built built${failed:+; failed:$failed}"
[ -z "$failed" ]
