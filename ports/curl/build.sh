#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - curl for SuckOS.
#
# http, https and the other plain transfer protocols, over the
# machine's own TCP stack and resolver. TLS is ports/openssl and its
# trust store is ports/ca-certs: /etc/ssl/cert.pem, which is both what
# --with-ca-bundle names below and OpenSSL's own default, so curl and
# every other OpenSSL program on the machine trust the same roots.
#
# The program is dynamic, against /lib/libc.so, like every other port;
# libcurl, OpenSSL, zlib and zstd are linked into it statically. There
# is no libcurl.so because nothing else on the machine links libcurl.
#
# WHAT IS OFF, and why:
#
#   --disable-threaded-resolver
#                 curl's threaded resolver exists so that a slow lookup
#                 can be timed out. The machine's getaddrinfo is the
#                 blocking one in libc/net, and its timeouts are its own
#                 retries; a thread per lookup buys nothing here
#   --disable-ipv6
#                 the network stack is IPv4 only
#   --disable-ldap, --disable-ldaps
#                 no LDAP library, and nobody fetches from LDAP with curl
#   --without-libssh2, --without-librtmp
#                 not ported: scp and sftp (there is dropbear's scp),
#                 and RTMP, which nobody needs
#   --disable-docs, --disable-manual
#                 the manual page needs a formatter on the build
#                 machine, and --manual compiled into the program is
#                 about 250 KB of text on a machine with little memory
#
# ON, each a port of its own:
#
#   zlib, zstd, brotli
#                 every compression a server offers for --compressed;
#                 brotli ("br") is what most of them prefer for text
#   nghttp2       HTTP/2, negotiated over TLS by ALPN
#   libidn2       international names: bücher.example is looked up as
#                 xn--bcher-kva.example
#   libpsl        the Public Suffix List, so a cookie cannot be scoped
#                 to all of .co.uk
#
# libidn2 and libpsl are static, so their own dependencies have to be
# named after them -- libpsl needs libidn2, and both need
# ports/libunistring -- and LIBS is the one place every link test sees.
# Nothing else would say so: there are no .pc files to read it from.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=8.22.0
# Checked against Daniel Stenberg's signature on the release
# (curl-VERSION.tar.xz.asc, key 27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2),
# not merely against itself.
SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
URL=https://curl.se/download/curl-$VERSION.tar.xz
SRC=$SRCDIR/curl-$VERSION
BUILD=$SRCDIR/build-curl-sage040
SSLOUT=$SRCDIR/build-openssl-sage040/sage040
ZOUT=$SRCDIR/build-zlib-sage040/sage040
ZSTDOUT=$SRCDIR/build-zstd-sage040/sage040
BROUT=$SRCDIR/build-brotli-sage040/sage040
NGOUT=$SRCDIR/build-nghttp2-sage040/sage040
IDNOUT=$SRCDIR/build-libidn2-sage040/sage040
PSLOUT=$SRCDIR/build-libpsl-sage040/sage040
UNIOUT=$SRCDIR/build-libunistring-sage040/sage040

for p in openssl zlib zstd brotli nghttp2 libunistring libidn2 libpsl; do
    "$HERE/../$p/build.sh" > /dev/null || {
        echo "curl: ports/$p/build.sh failed" >&2; exit 1; }
done

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/curl-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    if command -v gpg > /dev/null; then
        sig=$tarball.asc
        [ -f "$sig" ] || curl -sL --fail -o "$sig" "$URL.asc" || true
        if [ -f "$sig" ]; then
            gpg --verify "$sig" "$tarball" 2>&1 \
                | grep -q '^gpg: Good signature' \
                && echo "curl: good signature" \
                || echo "curl: signature NOT verified (no key?)" >&2
        fi
    fi
    tar -C "$SRCDIR" -xJf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host="$HOST_TRIPLET" \
        --build="$(cc -dumpmachine)" \
        --prefix=/usr \
        --disable-shared --enable-static \
        --with-openssl="$SSLOUT" \
        --with-zlib="$ZOUT" \
        --with-zstd="$ZSTDOUT" \
        --with-ca-bundle=/etc/ssl/cert.pem \
        --without-ca-path \
        --disable-threaded-resolver \
        --disable-ipv6 \
        --disable-ldap --disable-ldaps \
        --with-brotli="$BROUT" \
        --with-nghttp2="$NGOUT" \
        --with-libidn2="$IDNOUT" \
        --with-libpsl="$PSLOUT" \
        --without-libssh2 --without-librtmp \
        --disable-docs --disable-manual \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        LIBS="-L$IDNOUT/lib -lidn2 -L$UNIOUT/lib -lunistring" \
        PKG_CONFIG="$(command -v pkg-config)" \
        PKG_CONFIG_LIBDIR=/nonexistent \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/include"
cp "$BUILD/src/curl" "$OUT/bin/curl"
cp "$BUILD/lib/.libs/libcurl.a" "$OUT/lib/"
cp -r "$SRC/include/curl" "$OUT/include/"

write_pc libcurl "$VERSION" -lcurl \
    "libssl libcrypto zlib libzstd libbrotlidec libnghttp2 libidn2 libpsl"

echo "curl $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/curl" | tail -1
