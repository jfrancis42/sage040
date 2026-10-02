#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - nghttp2 for SuckOS: the HTTP/2 library, and nothing else.
#
# It is here for curl, which speaks HTTP/2 over TLS when it has this --
# negotiated by ALPN, so a server that prefers h2 gets it and one that
# does not is spoken to in HTTP/1.1 exactly as before.
#
# --enable-lib-only: nghttp2's tools (nghttp, nghttpd, h2load) are C++
# and want libev and libevent; the cross toolchain has no C++ front end
# for ports, and curl is the HTTP/2 client this machine needs.
#
# The hash is the one nghttp2 publish in checksums.txt beside the
# release.

set -eu

cd "$(dirname "$0")"
. ../cross.sh

VERSION=1.70.0
SHA256=e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
URL=https://github.com/nghttp2/nghttp2/releases/download/v$VERSION/nghttp2-$VERSION.tar.xz
SRC=$SRCDIR/nghttp2-$VERSION
BUILD=$SRCDIR/build-nghttp2-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/nghttp2-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xJf "$tarball"
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" \
        --prefix=/usr \
        --enable-lib-only \
        --disable-shared --enable-static \
        --disable-python-bindings \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD/lib" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include/nghttp2"
cp "$BUILD/lib/.libs/libnghttp2.a" "$OUT/lib/"
cp "$SRC/lib/includes/nghttp2/nghttp2.h" "$OUT/include/nghttp2/"
cp "$BUILD/lib/includes/nghttp2/nghttp2ver.h" "$OUT/include/nghttp2/"

write_pc libnghttp2 "$VERSION" -lnghttp2

echo "nghttp2 $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" -t "$OUT/lib/libnghttp2.a" | tail -1
