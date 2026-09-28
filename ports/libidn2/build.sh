#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU libidn2 for SuckOS: internationalised domain names
# (IDNA2008), and the `idn2` command.
#
# What turns "bücher.example" into "xn--bcher-kva.example" before it is
# looked up, for curl and wget -- and libpsl uses it to put the public
# suffix list into the same form.
#
# Against ports/libunistring rather than the private subset libidn2 can
# carry: libpsl needs the real library anyway, and two copies of the
# same Unicode tables in one program is 2 MB for nothing.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=2.3.8
SHA256=f557911bf6171621e1f72ff35f5b1825bb35b52ed45325dcdee931e5d3c0787a
URL=https://ftp.gnu.org/gnu/libidn/libidn2-$VERSION.tar.gz
SRC=$SRCDIR/libidn2-$VERSION
BUILD=$SRCDIR/build-libidn2-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/libidn2-$VERSION.tar.gz
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
                || { echo "libidn2: BAD signature" >&2; exit 1; }
            echo "libidn2: good signature (Simon Josefsson, GNU keyring)"
        fi
    fi
    tar -C "$SRCDIR" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

UNIOUT=$SRCDIR/build-libunistring-sage040/sage040
[ -f "$UNIOUT/lib/libunistring.a" ] || "$HERE/../libunistring/build.sh"

# See ports/libunistring/build.sh.
export gt_cv_func_uselocale_works=no

libc_fresh "$BUILD" || true

# AN INCREMENTAL REBUILD CAN FAIL, AND A CLEAN ONE CANNOT.
# libidn2 keeps a private helper archive that is ALSO called
# libunistring, and links it with -lunistring for the real one. On any
# RELINK of that helper -- a new libunistring, new libc headers, a
# touched source -- libtool resolves -lunistring to the helper's own
# previous copy, deletes it to rebuild it, and then fails to read it
# ("ar: .../unistring/.libs/libunistring.a: No such file or
# directory"). A fresh build has no previous copy and never sees the
# collision. Catching every trigger in advance was tried and missed one,
# so: try the incremental build, and if it fails, rebuild once from
# nothing (about fifteen seconds).
configure_it() {
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host="$HOST_TRIPLET" \
        --build="$("$SRC/build-aux/config.guess")" \
        --prefix=/usr \
        --disable-shared --enable-static \
        --disable-nls --disable-doc --disable-gtk-doc \
        --with-libunistring-prefix="$UNIOUT" \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
}

# The library and the command; not tests/, for the reason in
# ports/libunistring.
make_it() {
    local d
    rm -f "$BUILD/make.log"
    for d in gl unistring lib src; do
        [ -d "$BUILD/$d" ] || continue
        make -C "$BUILD/$d" -j"$(nproc)" >> "$BUILD/make.log" 2>&1 || return 1
    done
}

[ -f "$BUILD/Makefile" ] || configure_it
if ! make_it; then
    echo "libidn2: incremental build failed; rebuilding from clean" >&2
    rm -rf "$BUILD"
    libc_fresh "$BUILD" || true
    configure_it
    make_it || { tail -30 "$BUILD/make.log"; exit 1; }
fi

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/bin"
cp "$BUILD/lib/.libs/libidn2.a" "$OUT/lib/"
cp "$BUILD/lib/idn2.h" "$OUT/include/" 2>/dev/null || cp "$SRC/lib/idn2.h" "$OUT/include/"
cp "$BUILD/src/idn2" "$OUT/bin/idn2"

echo "libidn2 $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/idn2" | tail -1
