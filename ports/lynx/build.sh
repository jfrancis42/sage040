#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - Lynx for SuckOS: a web browser for the terminal.
#
# Over the pieces curl and wget already use: ports/openssl for https
# (trusting ports/ca-certs' /etc/ssl/cert.pem), ports/zlib, ports/bzip2
# and ports/brotli for compressed pages -- as LIBRARIES: without
# --with-brotli, Lynx still offers "br" (it assumes a `brotli` program
# is on PATH, because of --disable-full-paths) and then fails on every
# site that takes it up, lynx.invisible-island.net among them, with
# "Error uncompressing temporary file!"), ports/libidn2 for international
# names. The screen is ports/ncurses, which is built WIDE (libncursesw),
# so --with-screen=ncursesw and --enable-widec give Lynx real UTF-8 on
# a terminal that has it -- a serial or ssh session to a modern
# terminal. The machine's own framebuffer console decodes no UTF-8 yet
# (curl-lynx.md), so on it, set Lynx's character set to one it can show.
#
# WHAT IS OFF, and why:
#
#   --disable-nls      no message catalogues on the machine
#   --disable-full-paths
#                      Lynx otherwise records the BUILD machine's
#                      absolute path for every helper it finds
#                      (/usr/bin/gzip, ...), which is nonsense on this
#                      one. Names alone are looked up in PATH at run time
#   IPv6               left at Lynx's default, off: the stack is IPv4
#
# -lbrotlicommon on LIBS: static libbrotlidec needs it after itself,
# and configure links -lbrotlidec alone ("Cannot find brotlidec").
#
# -ltinfow on LIBS: ports/ncurses is built --with-termlib, so the
# terminfo half (acs_map, tigetstr, ...) is a library of its own, and
# Lynx's configure links -lncursesw without it -- every curses function
# that draws a line then "does not exist" and color-styles are refused.
#
# makeuctb, which turns the character-set tables into C, runs on the
# HOST while building, so BUILD_CC is the host's compiler.
#
# Checked against Thomas Dickey's signature, with his key as he
# publishes it (fingerprint pinned below) -- the same maintainer as
# ncurses.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=2.9.3
SHA256=174b7f2866a60f3247ba75f5c7dbb10b124aede4a1359312de15f3bfebd2050f
URL=https://invisible-mirror.net/archives/lynx/tarballs/lynx$VERSION.tar.bz2
KEYURL=https://invisible-island.net/public/dickey@invisible-island.net-rsa3072.asc
KEYFPR=19882D92DDA4C400C22C0D56CC2AF4472167BE03
SRC=$SRCDIR/lynx$VERSION
BUILD=$SRCDIR/build-lynx-sage040

SSLOUT=$SRCDIR/build-openssl-sage040/sage040
ZOUT=$SRCDIR/build-zlib-sage040/sage040
BZOUT=$SRCDIR/build-bzip2-sage040/sage040
NCOUT=$SRCDIR/build-ncurses-sage040/sage040
BROUT=$SRCDIR/build-brotli-sage040/sage040
UNIOUT=$SRCDIR/build-libunistring-sage040/sage040
IDNOUT=$SRCDIR/build-libidn2-sage040/sage040

for p in openssl zlib bzip2 brotli ncurses libunistring libidn2; do
    "$HERE/../$p/build.sh" > /dev/null || {
        echo "lynx: ports/$p/build.sh failed" >&2; exit 1; }
done

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/lynx$VERSION.tar.bz2
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    if command -v gpg > /dev/null; then
        sig=$tarball.asc
        key=$SRCDIR/dickey-$KEYFPR.asc
        ring=$SRCDIR/dickey.kbx
        [ -f "$sig" ] || curl -sL --fail -o "$sig" "$URL.asc" || true
        [ -f "$key" ] || curl -sL --fail -o "$key" "$KEYURL" || true
        if [ -f "$sig" ] && [ -f "$key" ]; then
            gpg --show-keys --with-colons "$key" 2>/dev/null \
                | grep -q "^fpr:::::::::$KEYFPR:" \
                || { echo "lynx: $key is not key $KEYFPR" >&2; exit 1; }
            gpg --no-default-keyring --keyring "$ring" --import "$key" 2>/dev/null
            gpg --no-default-keyring --keyring "$ring" \
                --verify "$sig" "$tarball" 2>&1 | grep -q '^gpg: Good signature' \
                || { echo "lynx: BAD signature" >&2; exit 1; }
            echo "lynx: good signature (Thomas E. Dickey, $KEYFPR)"
        fi
    fi
    tar -C "$SRCDIR" -xjf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/makefile" ] && [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" \
        --host="$HOST_TRIPLET" \
        --build="$("$SRC/config.guess")" \
        --prefix=/usr --sysconfdir=/etc \
        --with-screen=ncursesw --enable-widec \
        --with-curses-dir="$NCOUT" \
        --with-ssl="$SSLOUT" \
        --with-zlib --with-bzlib --with-brotli \
        --enable-idna \
        --enable-default-colors --enable-color-style \
        --enable-externs --enable-nested-tables \
        --disable-nls --disable-full-paths \
        --with-build-cc=cc \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS" \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        CPPFLAGS="-I$ZOUT/include -I$BZOUT/include -I$BROUT/include -I$IDNOUT/include" \
        LDFLAGS="-L$ZOUT/lib -L$BZOUT/lib -L$BROUT/lib" \
        LIBS="-L$BROUT/lib -lbrotlicommon -L$NCOUT/lib -ltinfow -L$IDNOUT/lib -lidn2 -L$UNIOUT/lib -lunistring" \
        PKG_CONFIG="$(command -v pkg-config)" \
        PKG_CONFIG_LIBDIR=/nonexistent \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { tail -40 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/etc"
cp "$BUILD/lynx" "$OUT/bin/lynx"
cp "$SRC/lynx.cfg" "$OUT/etc/lynx.cfg"
cp "$SRC/samples/lynx.lss" "$OUT/etc/lynx.lss"

echo "lynx $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/lynx" | tail -1
