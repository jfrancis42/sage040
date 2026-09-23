#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU gettext's RUNTIME for SuckOS (task 46).
#
# libintl: the library behind the `_()` that every GNU program is
# written around. CLISP requires it; so does anything else that wants
# its messages translated.
#
# THE RUNTIME ONLY, not the tools. gettext ships two halves:
# gettext-runtime, which is libintl and `gettext(1)` -- a few hundred
# kilobytes -- and gettext-tools, which is xgettext, msgfmt, msgmerge
# and a great deal of machinery for EXTRACTING strings from source in
# a dozen languages. The tools are a developer's, not a machine's:
# catalogues are compiled on a workstation and the .mo files copied
# over. Building them here would mean porting their gnulib, which has
# already been the most troublesome thing in this tree (see
# ports/gcc/patches/01, where gcc's bundled copy had to be switched
# off entirely).
#
# Signed by Bruno Haible and checked against GNU's keyring.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=0.23.1
SHA256=52a578960fe308742367d75cd1dff8552c5797bd0beba7639e12bdcda28c0e49
URL=https://ftp.gnu.org/gnu/gettext/gettext-$VERSION.tar.gz
SRC=$SRCDIR/gettext-$VERSION
BUILD=$SRCDIR/build-gettext-sage040
ICONVOUT=$SRCDIR/build-libiconv-sage040/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/gettext-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    if command -v gpg > /dev/null; then
        sig=$tarball.sig
        ring=$SRCDIR/gnu-keyring.gpg
        [ -f "$sig" ] || curl -sL --fail -o "$sig" "$URL.sig" || true
        [ -f "$ring" ] || \
            curl -sL --fail -o "$ring" https://ftp.gnu.org/gnu/gnu-keyring.gpg || true
        if [ -f "$sig" ] && [ -f "$ring" ]; then
            gpg --no-default-keyring --keyring "$ring" \
                --verify "$sig" "$tarball" 2>&1 | grep -q '^gpg: Good signature' \
                || { echo "gettext: BAD signature" >&2; exit 1; }
            echo "gettext: good signature (Bruno Haible, GNU keyring)"
        fi
    fi
    tar -C "$SRCDIR" -xzf "$tarball"
fi

[ -f "$ICONVOUT/lib/libiconv.a" ] || "$HERE/../libiconv/build.sh"

# See ports/gcc/build.sh: this gnulib decides uselocale() is usable
# from a compile test and then calls uselocale(NULL), which this
# locale_t is not a pointer for.
export gt_cv_func_uselocale_works=no
export ac_cv_tls=none

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/gettext-runtime/configure" \
        --host=m68k-unknown-elf \
        --build="$("$SRC/build-aux/config.guess")" \
        --prefix=/usr \
        --disable-shared --enable-static \
        --disable-java \
        --disable-csharp \
        --disable-libasprintf \
        --disable-openmp \
        --without-emacs \
        --with-libiconv-prefix="$ICONVOUT" \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        CC_FOR_BUILD=cc \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT" "$BUILD/inst"
make -C "$BUILD" install DESTDIR="$BUILD/inst" > "$BUILD/install.log" 2>&1 \
    || { tail -20 "$BUILD/install.log"; exit 1; }
mkdir -p "$OUT"
cp -a "$BUILD/inst/usr/lib" "$BUILD/inst/usr/include" "$OUT/" 2>/dev/null || true
[ -d "$BUILD/inst/usr/bin" ] && cp -a "$BUILD/inst/usr/bin" "$OUT/"
rm -f "$OUT/lib"/*.la

echo "gettext(runtime) $VERSION -> $OUT"
