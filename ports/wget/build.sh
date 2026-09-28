#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU Wget for SuckOS.
#
# The other way to fetch things, and the one scripts and recursive
# mirroring expect. Over the same pieces as curl: ports/openssl for TLS
# (trusting ports/ca-certs' /etc/ssl/cert.pem, OpenSSL's default),
# ports/zlib, ports/libidn2 for international names and ports/libpsl for
# cookie scoping.
#
# WHAT IS OFF, and why:
#
#   --disable-pcre2, --disable-pcre
#                 regular expressions in --accept-regex; POSIX regex
#                 from libc still works for them
#   --without-libuuid
#                 only WARC output uses it, and wget makes its own
#   --disable-ipv6
#                 the network stack is IPv4 only (as for curl)
#   --disable-nls no message catalogues on the machine
#   --disable-threads
#                 wget is single-threaded; this only switches off
#                 gnulib's locking, which this system does not need
#
# PKG-CONFIG, as in ports/libpsl: configure must find one, but must not
# find the HOST's libraries, so it looks in an empty directory and each
# library is named directly through its *_CFLAGS / *_LIBS variables.
# libpsl and libidn2 are static, so their own dependencies follow them.
# They are NOT on LIBS: libidn2.a carries gnulib's error() inside it,
# and with it on every link test configure concluded the C library has
# error() -- then gnulib's error.h wrapped a function no header
# declares, and lib/ would not compile.
# OpenSSL is ALSO on CPPFLAGS/LDFLAGS: --with-openssl=yes (hashes from
# libcrypto) is tested with a bare AC_CHECK_LIB that never reads
# OPENSSL_LIBS, and fails as "openssl development library not found".

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.25.0
SHA256=766e48423e79359ea31e41db9e5c289675947a7fcf2efdcedb726ac9d0da3784
URL=https://ftp.gnu.org/gnu/wget/wget-$VERSION.tar.gz
SRC=$SRCDIR/wget-$VERSION
BUILD=$SRCDIR/build-wget-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/wget-$VERSION.tar.gz
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
                || { echo "wget: BAD signature" >&2; exit 1; }
            echo "wget: good signature (Darshit Shah, GNU keyring)"
        fi
    fi
    tar -C "$SRCDIR" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

SSLOUT=$SRCDIR/build-openssl-sage040/sage040
ZOUT=$SRCDIR/build-zlib-sage040/sage040
UNIOUT=$SRCDIR/build-libunistring-sage040/sage040
IDNOUT=$SRCDIR/build-libidn2-sage040/sage040
PSLOUT=$SRCDIR/build-libpsl-sage040/sage040

for p in openssl zlib libunistring libidn2 libpsl; do
    "$HERE/../$p/build.sh" > /dev/null || {
        echo "wget: ports/$p/build.sh failed" >&2; exit 1; }
done

# See ports/libunistring/build.sh.
export gt_cv_func_uselocale_works=no

IDNLIBS="-L$IDNOUT/lib -lidn2 -L$UNIOUT/lib -lunistring"

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host="$HOST_TRIPLET" \
        --build="$("$SRC/build-aux/config.guess")" \
        --prefix=/usr --sysconfdir=/etc \
        --with-ssl=openssl --with-openssl=yes \
        --with-libpsl --with-zlib \
        --disable-pcre2 --disable-pcre \
        --without-libuuid --without-metalink --without-cares \
        --disable-nls --disable-threads --disable-ipv6 \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        PKG_CONFIG="$(command -v pkg-config)" \
        PKG_CONFIG_LIBDIR=/nonexistent \
        OPENSSL_CFLAGS="-I$SSLOUT/include" \
        OPENSSL_LIBS="-L$SSLOUT/lib -lssl -lcrypto" \
        ZLIB_CFLAGS="-I$ZOUT/include" \
        ZLIB_LIBS="-L$ZOUT/lib -lz" \
        LIBIDN2_CFLAGS="-I$IDNOUT/include" \
        LIBIDN2_LIBS="$IDNLIBS" \
        LIBPSL_CFLAGS="-I$PSLOUT/include" \
        LIBPSL_LIBS="-L$PSLOUT/lib -lpsl $IDNLIBS" \
        CPPFLAGS="-I$SSLOUT/include" \
        LDFLAGS="-L$SSLOUT/lib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

# The program only: lib/ (gnulib) and src/. Not doc/, which wants
# pod2man, nor tests/.
rm -f "$BUILD/make.log"
for d in lib src; do
    make -C "$BUILD/$d" -j"$(nproc)" >> "$BUILD/make.log" 2>&1 \
        || { tail -30 "$BUILD/make.log"; exit 1; }
done

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$BUILD/src/wget" "$OUT/bin/wget"

echo "wget $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/wget" | tail -1
