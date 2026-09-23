#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - OpenSSL for SuckOS.
#
# The biggest thing this brings is not TLS, it is HASHES THAT ARE RIGHT.
# CPython's bundled HACL* code computes MD5 wrongly on a big-endian
# machine -- its htole64 does two htobe32s, which on this CPU is the
# identity -- and ports/python/patches/04 fixes that by hand. With
# OpenSSL here, hashlib uses OpenSSL's implementations instead, which
# have been run against the published test vectors on big-endian
# machines for twenty-five years. Then TLS: `_ssl`, and so urllib over
# https, pip's transport, and the `openssl` command for looking at a
# certificate.
#
# WHAT IS TURNED OFF, and why each one is a property of this machine
# rather than a preference:
#
#   no-shared    there is a dynamic loader, but OpenSSL's shared build
#                wants versioned sonames and symbol maps; the static
#                library is what CPython links anyway
#   no-dso       ld.so has no dlopen, so a provider cannot be loaded
#                from a file. The default and legacy providers are
#                built in
#   no-asm       every line of OpenSSL's assembly is for some other
#                CPU. There is no m68k path to lose
#   no-secure-memory
#                it wants mmap of anonymous memory it can mlock, and
#                this kernel has no mlock. Without it OpenSSL keeps
#                keys in ordinary heap, which is what it does on any
#                platform that cannot lock pages
#   no-afalgeng, no-ktls
#                Linux kernel crypto and kernel TLS: neither exists here
#   no-tests     the test suite is a host program per test
#
# -DB_ENDIAN IS NOT LOAD-BEARING, and this comment used to say the
# opposite. The reasoning was that OpenSSL's generic targets do not
# state a byte order, so they must assume the wrong one and SHA-256
# would quietly return a different number. It was measured instead of
# believed: the same source configured WITHOUT the flag was built and
# run on the machine beside this one, and both agree with each other
# and with the host's coreutils on SHA-256 and MD5. OpenSSL 3.5
# determines byte order for itself where it matters.
#
# The flag stays because it is true of this machine and costs nothing,
# not because anything depends on it. The digest checks in
# kernel/pylibtest.sh are worth having for their own reason -- they
# compare against a different implementation on a different CPU -- but
# they are not evidence about this flag, and a passing suite must not
# be read as proof that removing it would break something.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=3.5.4
# OpenSSL publish this beside the tarball, as openssl-VERSION.tar.gz.sha256.
SHA256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
URL=https://github.com/openssl/openssl/releases/download/openssl-$VERSION/openssl-$VERSION.tar.gz
SRC=$SRCDIR/openssl-$VERSION
BUILD=$SRCDIR/build-openssl-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/openssl-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# The machine's own Configure target. Configure reads every
# Configurations/*.conf in the source tree, so it goes there.
#
# ONLY IF IT CHANGED. OpenSSL's Makefile watches its configuration
# files and, when one is newer, reconfigures and then STOPS with
# "Please run the same make command again" -- so copying this
# unconditionally made every build after the first one fail without
# compiling anything.
if ! cmp -s "$HERE/50-sage040.conf" "$SRC/Configurations/50-sage040.conf"; then
    cp "$HERE/50-sage040.conf" "$SRC/Configurations/50-sage040.conf"
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && perl "$SRC/Configure" sage040 \
        --prefix=/usr --openssldir=/etc/ssl \
        no-shared no-dso no-asm no-tests \
        no-afalgeng no-ktls no-secure-memory no-sctp \
        --banner="OpenSSL for SuckOS" \
        --with-rand-seed=devrandom \
        CC="$CROSS_CC" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        CPPFLAGS="$CROSS_CPPFLAGS" \
        CFLAGS="$CROSS_CFLAGS -DB_ENDIAN" \
        LDFLAGS="$STATIC_LDFLAGS" \
        LDLIBS="$STATIC_LIBS" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

# The libraries first: they are what CPython needs, and they build
# whether or not the command does.
make -C "$BUILD" -j8 build_libs > "$BUILD/libs.log" 2>&1 || {
    # The one failure that is not a failure: a reconfiguration asks to
    # be run again. Anything else is real.
    if grep -q "run the same make command again" "$BUILD/libs.log"; then
        make -C "$BUILD" -j8 build_libs \
            > "$BUILD/libs.log" 2>&1 || { tail -30 "$BUILD/libs.log"; exit 1; }
    else
        tail -30 "$BUILD/libs.log"; exit 1
    fi
}

OUT=$BUILD/sage040
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/bin"
cp "$BUILD/libcrypto.a" "$BUILD/libssl.a" "$OUT/lib/"
rm -rf "$OUT/include/openssl"
mkdir -p "$OUT/include/openssl"
cp "$SRC/include/openssl/"*.h "$OUT/include/openssl/"
cp "$BUILD/include/openssl/"*.h "$OUT/include/openssl/"

# The command, against /lib/libc.so. Not fatal if it will not link:
# the libraries are the reason this port exists.
if make -C "$BUILD" -j8 build_programs \
       LDFLAGS="$DYN_LDFLAGS" LDLIBS="$DYN_LIBS" \
       > "$BUILD/apps.log" 2>&1; then
    cp "$BUILD/apps/openssl" "$OUT/bin/openssl"
else
    echo "openssl: the command did not link; the libraries are built." >&2
    tail -8 "$BUILD/apps.log" >&2
fi

echo "openssl $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" -t "$OUT/lib/libcrypto.a" 2>/dev/null | tail -1
