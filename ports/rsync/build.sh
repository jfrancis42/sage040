#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - rsync for SuckOS (task 42).
#
# rsync over ssh, which is the way anybody actually uses it: it runs
# `ssh host rsync --server ...` and talks its own protocol down the
# pipe. That is why it comes after Dropbear, and why the client is
# installed as `ssh` as well as `dbclient` -- rsync spells it that way
# and has no idea what dbclient is.
#
# WHAT IS OFF, and every one of them is a property of this system:
#
#   --disable-acl-support     a FAT volume has no access control list
#   --disable-xattr-support   nor extended attributes
#   --disable-iconv           libiconv is not built (task 45); names
#                             here are UTF-8 bytes and stay so
#   --disable-md2man          a manual page formatter that wants perl
#                             on the BUILD machine to run at all
#   --disable-lz4             not built for this machine
#   --disable-openssl         rsync uses it only for MD4/MD5 in the
#                             protocol, and its own copies are correct
#                             and smaller than pulling libcrypto into
#                             a program that otherwise needs none
#
# zstd is LEFT ON: it is what rsync 3.2 and later negotiate by
# preference for compression, and the library is built for this
# machine.
#
# --disable-xxhash only because there is no xxhash port yet. It is a
# checksum, not a feature -- rsync falls back to MD5 and transfers the
# same bytes, more slowly. xxhash is one C file and would be an easy
# port; it is listed in progress.md rather than done here.
#
# The bundled zlib and popt are used rather than ports/zlib: rsync's
# zlib carries its own patches for the protocol's history, and using a
# different one is documented by rsync's own authors as producing
# incompatible output.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=3.4.1
# Checked against Andrew Tridgell's signature on the release
# (rsync-VERSION.tar.gz.asc), not merely against itself.
SHA256=2924bcb3a1ed8b551fc101f740b9f0fe0a202b115027647cf69850d65fd88c52
URL=https://download.samba.org/pub/rsync/src/rsync-$VERSION.tar.gz
SRC=$SRCDIR/rsync-$VERSION
BUILD=$SRCDIR/build-rsync-sage040
ZSTDOUT=$SRCDIR/build-zstd-sage040/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/rsync-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    if command -v gpg > /dev/null; then
        sig=$tarball.asc
        [ -f "$sig" ] || curl -sL --fail -o "$sig" "$URL.asc" || true
        if [ -f "$sig" ]; then
            gpg --verify "$sig" "$tarball" 2>&1 \
                | grep -q '^gpg: Good signature' \
                && echo "rsync: good signature" \
                || echo "rsync: signature NOT verified (no key?)" >&2
        fi
    fi
    tar -C "$SRCDIR" -xzf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host=m68k-unknown-elf \
        --build="$(cc -dumpmachine)" \
        --prefix=/usr \
        --disable-acl-support \
        --disable-xattr-support \
        --disable-iconv \
        --disable-iconv-open \
        --disable-md2man \
        --disable-lz4 \
        --disable-xxhash \
        --disable-openssl \
        --with-included-zlib=yes \
        --with-included-popt=yes \
        --with-rsh="ssh" \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS \
-I$ZSTDOUT/include" \
        CC_FOR_BUILD=cc \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        LDFLAGS="-L$ZSTDOUT/lib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$BUILD/rsync" "$OUT/bin/rsync"

echo "rsync $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/rsync" | tail -1
