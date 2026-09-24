#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - the patched QEMU, from a pristine tarball.
#
# README.md next to this file says what the patch does and why. This
# does it: fetch, apply, configure, build, install, and then CHECK the
# two things that have actually gone wrong here before.
#
# It exists because the procedure was a block of shell in that README
# to be typed by hand, which made the emulator the one part of this
# project that only existed on whichever machine somebody had last
# built it on. The consequence is worth stating plainly: after adding a
# device to the machine model, every host that runs this needs its QEMU
# rebuilt -- and a host whose source tree has been deleted since the
# last build has to re-fetch the tarball first. That is now one command.
#
#   ./build.sh          build and install into ~/m68k/sage040-qemu
#   ./build.sh clean    remove the build directory
#
# Override the destination with SAGE_QEMU, the source area with
# SAGE_SRC.

set -u
cd "$(dirname "$0")"
HERE=$(pwd)

VERSION=11.1.1
# The release signed by QEMU's release manager: "Good signature from
# Michael Roth <michael.roth@amd.com>", RSA key
# CEACC9E15534EBABB82D3FA03353C9CEF108B584. Change VERSION and you must
# change SHA256 with it -- and re-read sage040.patch, which is against
# this version's files.
SHA256=079ffbff8a7111bbc89022107cbabf3bbfd614d5fc9d7cc675991196aca12482
URL=https://download.qemu.org/qemu-$VERSION.tar.xz

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SRC=$SRCDIR/qemu-$VERSION
BUILD=$SRCDIR/build-qemu-sage040
PREFIX=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}

if [ "${1:-}" = clean ]; then
    rm -rf "$BUILD"
    echo "removed $BUILD"
    exit 0
fi

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/qemu-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
fi

# The three new files, then the edits to the existing ones. Tracked in
# the source tree so a second run does not try to patch twice -- `patch
# -N` would refuse, and refusing looks like a failure rather than like
# nothing needing to be done.
#
# A TREE THAT IS ALREADY PATCHED AND HAS NO MARKER IS THE NORMAL CASE
# on the machine where this was developed: the tree was patched by hand
# long before there was a script. So the marker is not trusted on its
# own -- the patch itself is asked whether it has already been applied,
# by seeing if it reverses cleanly. Getting this wrong means running
# `make qemu` on the original machine and mangling the one tree that
# was known to work.
applied=$SRC/.sage040-applied
if [ ! -f "$applied" ]; then
    if patch -d "$SRC" -p1 -R --dry-run -s -f < "$HERE/sage040.patch" \
         > /dev/null 2>&1; then
        echo "$SRC was already patched; marking it"
        touch "$applied"
    else
        cp "$HERE/new-files/hw-m68k-sage040.c"         "$SRC/hw/m68k/sage040.c"
        cp "$HERE/new-files/hw-misc-mc68901.c"         "$SRC/hw/misc/mc68901.c"
        cp "$HERE/new-files/include-hw-misc-mc68901.h" "$SRC/include/hw/misc/mc68901.h"
        patch -d "$SRC" -p1 -N -s < "$HERE/sage040.patch" \
            || { echo "qemu: the patch did not apply to $SRC" >&2; exit 1; }
        touch "$applied"
        echo "patched $SRC"
    fi
fi

if [ ! -f "$BUILD/build.ninja" ]; then
    mkdir -p "$BUILD"
    ( cd "$BUILD" && "$SRC/configure" \
        --target-list=m68k-softmmu \
        --prefix="$PREFIX" \
        --enable-slirp \
        --disable-docs --disable-werror --disable-guest-agent \
        --disable-tools --disable-vnc --disable-spice \
        > configure.log 2>&1 ) \
        || { tail -40 "$BUILD/configure.log"; exit 1; }
fi

ninja -C "$BUILD" > "$BUILD/build.log" 2>&1 \
    || { tail -40 "$BUILD/build.log"; exit 1; }
ninja -C "$BUILD" install >> "$BUILD/build.log" 2>&1 \
    || { tail -40 "$BUILD/build.log"; exit 1; }

# --- the two things that have gone wrong here -------------------------
#
# CONFIG_PCI must stay UNSET. Selecting PCI to fix an SM501 link error
# is the wrong fix and was explicitly rejected: this design is meant to
# be buildable in hardware and will never have a PCI bus. The right fix
# is the #ifdef CONFIG_PCI guard in the patch, and if PCI has crept
# back in then something selected it and the guard is moot.
if grep -q CONFIG_PCI "$BUILD/m68k-softmmu-config-devices.mak" 2>/dev/null; then
    echo "qemu: CONFIG_PCI is SET, and it must not be." >&2
    echo "      Something in the patch selected it; see qemu-patch/README.md." >&2
    exit 1
fi

# And that the machine is actually there. A QEMU that builds but has no
# sage040 is a QEMU every test in this tree will fail against, with a
# message about the machine type rather than about this.
if ! "$PREFIX/bin/qemu-system-m68k" -M help 2>/dev/null | grep -q '^sage040'; then
    echo "qemu: built and installed, but has no sage040 machine." >&2
    exit 1
fi

echo "qemu $VERSION -> $PREFIX"
"$PREFIX/bin/qemu-system-m68k" -M help | grep '^sage040'
