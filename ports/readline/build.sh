#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU readline for SuckOS.
#
# What this buys: CPython's interactive prompt gets line editing, a
# history that persists, and tab completion -- the difference between
# a Python you can try something in and one you retype everything at.
# The kernel's own shell has its own editor (kernel/edit.c, in the
# shell where bash keeps one); readline is for the PROGRAMS.
#
# It is here as a CPython dependency. The same library is what CLISP
# will want later, but nothing is built for CLISP yet.
#
# THE TARBALL IS CHECKED AGAINST CHET RAMEY'S SIGNATURE, not against a
# hash of my own computing: readline is signed, GNU publish the signing
# keys in gnu-keyring.gpg, and a detached signature says the authors
# produced these bytes. A recorded sha256 would only say they have not
# changed since I first fetched them. The hash is kept as well, so an
# offline rebuild still has something to check.
#
# Linked against libtinfow from ports/ncurses for the terminal
# description; readline does its own screen work above that.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=8.3
SHA256=fe5383204467828cd495ee8d1d3c037a7eba1389c22bc6a041f627976f9061cc
URL=https://ftp.gnu.org/gnu/readline/readline-$VERSION.tar.gz
SRC=$SRCDIR/readline-$VERSION
BUILD=$SRCDIR/build-readline-sage040
NCOUT=$SRCDIR/build-ncurses-sage040/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/readline-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -

    # And the signature, when gpg and the network are both here. A
    # failure to FETCH is not a failure to verify, so it is not fatal;
    # a signature that is present and BAD is.
    if command -v gpg > /dev/null; then
        sig=$tarball.sig
        ring=$SRCDIR/gnu-keyring.gpg
        [ -f "$sig" ] || curl -sL --fail -o "$sig" "$URL.sig" || true
        [ -f "$ring" ] || \
            curl -sL --fail -o "$ring" https://ftp.gnu.org/gnu/gnu-keyring.gpg || true
        if [ -f "$sig" ] && [ -f "$ring" ]; then
            gpg --no-default-keyring --keyring "$ring" \
                --verify "$sig" "$tarball" 2>&1 | grep -q '^gpg: Good signature' \
                || { echo "readline: BAD signature on $tarball" >&2; exit 1; }
            echo "readline: good signature (Chet Ramey, GNU keyring)"
        fi
    fi

    tar -C "$SRCDIR" -xzf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

if [ ! -f "$NCOUT/lib/libtinfow.a" ]; then
    "$HERE/../ncurses/build.sh"
fi

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" \
        --disable-shared --enable-static \
        --with-curses \
        --disable-install-examples \
        CPPFLAGS="$CROSS_CPPFLAGS -I$NCOUT/include" \
        LDFLAGS="$STATIC_LDFLAGS -L$NCOUT/lib" \
        LIBS="-ltinfow $STATIC_LIBS" \
        bash_cv_wcwidth_broken=no \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

# The libraries only. readline's `all` also builds the examples, which
# want a program on the machine to run; nothing here reads them.
make -C "$BUILD" -j8 libreadline.a libhistory.a \
    > "$BUILD/make.log" 2>&1 || { tail -30 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
mkdir -p "$OUT/lib" "$OUT/include/readline"
cp "$BUILD/libreadline.a" "$BUILD/libhistory.a" "$OUT/lib/"
for h in readline.h chardefs.h keymaps.h history.h tilde.h rlstdc.h \
         rlconf.h rltypedefs.h; do
    cp "$SRC/$h" "$OUT/include/readline/"
done

echo "readline $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" -t "$OUT/lib/libreadline.a" 2>/dev/null | tail -1
