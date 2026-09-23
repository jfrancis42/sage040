#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - Dropbear: ssh, sshd and scp for SuckOS (tasks 41, 42).
#
# WHY DROPBEAR AND NOT OPENSSH. This was a real choice and it is worth
# writing down rather than discovering later from the Makefile.
#
# OpenSSH is the reference implementation, has more in it (sftp, an
# agent, ProxyCommand, certificates), and this machine now has OpenSSL,
# which OpenSSH wants. Against that: OpenSSH's privilege separation
# expects to fork a child, setuid it to a dedicated unprivileged user
# and chroot it into an empty directory -- a design built around a
# filesystem that can hold an owner and a permission, which this one
# cannot (task 36). Running it with privilege separation off is
# possible and is precisely the configuration its authors warn about.
#
# Dropbear was written for machines this size. It carries its own
# crypto, so it does not have to agree with OpenSSL about anything; it
# is one binary that is both client and server; and it bundles scp,
# which was asked for. The protocol is the same protocol, so an
# OpenSSH client talks to this server and this client talks to an
# OpenSSH server -- which is the part that actually matters.
#
# If OpenSSH is wanted later, nothing here stands in the way.
#
# -D__STDC_WANT_LIB_EXT1__=1 because Dropbear wipes key material with
# memset_s(), which is C11's Annex K and is exactly the right thing to
# use: an ordinary memset over memory nothing reads again is something
# a compiler is allowed to delete. picolibc HAS memset_s and hides its
# declaration behind that macro, as the standard says to, so without
# it the call compiled as an implicit declaration and the build
# stopped.
#
# PASSWORD AUTHENTICATION IS OFF, and that is not a shortcut either.
# Verifying a password means crypt(3), which picolibc does not have,
# and inventing one badly is worse than not having one. Public keys
# work, which is the authentication anybody should be using anyway;
# `dropbearkey` makes them. If passwords are wanted, crypt() belongs in
# the C library and is written up in progress.md.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=2026.94
# Fetched over TLS from the project's own release directory.
#
# THE SIGNATURE COULD NOT BE CHECKED, and saying so is the point: the
# tarball is signed (dropbear-VERSION.tar.bz2.asc, RSA key
# F7347EF2EE2E07A267628CA944931494F29C6773), and that key is not on the
# keyserver this machine can reach, so there was nothing to verify it
# against. This hash therefore pins the bytes that arrived, which is a
# weaker claim than readline's -- readline's signature IS checked,
# against GNU's own keyring. Worth fixing by fetching the key by hand
# and recording its fingerprint here.
SHA256=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
URL=https://matt.ucc.asn.au/dropbear/releases/dropbear-$VERSION.tar.bz2
SRC=$SRCDIR/dropbear-$VERSION
BUILD=$SRCDIR/build-dropbear-sage040
ZOUT=$SRCDIR/build-zlib-sage040/sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/dropbear-$VERSION.tar.bz2
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xjf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

libc_fresh "$BUILD" || true
mkdir -p "$BUILD"

# What this system can and cannot do, said once, in the file Dropbear
# reads for exactly that purpose.
#
# IT GOES IN THE BUILD DIRECTORY, not beside the source: Dropbear's
# Makefile tests `$(wildcard ./localoptions.h)` and defines
# LOCALOPTIONS_H_EXISTS from it, and "." is where make is running. Put
# in the source tree it was never seen, and the build stopped on
# "DROPBEAR_SVR_PASSWORD_AUTH requires `crypt()'" -- the default it
# was supposed to be overriding.
cp "$HERE/localoptions.h" "$BUILD/localoptions.h"

if [ ! -f "$BUILD/Makefile" ]; then
    (cd "$BUILD" && "$SRC/configure" \
        --host=m68k-unknown-elf \
        --build="$(cc -dumpmachine)" \
        --prefix=/usr \
        --disable-zlib \
        --disable-lastlog \
        --disable-utmp --disable-utmpx \
        --disable-wtmp --disable-wtmpx \
        --disable-loginfunc \
        --disable-pam \
        --disable-harden \
        CC="$CROSS_CC $CROSS_CFLAGS $CROSS_CPPFLAGS $SPECS_CFLAGS \
-D__STDC_WANT_LIB_EXT1__=1" \
        CC_FOR_BUILD=cc \
        AR="$CROSS_BIN/m68k-elf-ar" \
        RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        > configure.log 2>&1) || { tail -30 "$BUILD/configure.log"; exit 1; }
fi

make -C "$BUILD" -j"$(nproc)" PROGRAMS="dropbear dbclient dropbearkey dropbearconvert scp" \
    > "$BUILD/make.log" 2>&1 || { tail -40 "$BUILD/make.log"; exit 1; }

OUT=$BUILD/sage040
rm -rf "$OUT"
mkdir -p "$OUT/bin"
for p in dropbear dbclient dropbearkey dropbearconvert scp; do
    [ -f "$BUILD/$p" ] && cp "$BUILD/$p" "$OUT/bin/$p"
done
# `ssh` as well as `dbclient`: rsync runs "ssh" by name and so does
# everybody's fingers. FAT has no symbolic links, so it is a copy.
[ -f "$OUT/bin/dbclient" ] && cp "$OUT/bin/dbclient" "$OUT/bin/ssh"

echo "dropbear $VERSION -> $OUT"
ls -l "$OUT/bin" | awk 'NR>1 {print "   ", $9, $5}'
