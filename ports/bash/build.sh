#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - GNU bash for SuckOS.
#
# Fetched from ftp.gnu.org at a fixed release, checked against a pinned
# SHA-256 (its GPG signature was checked against the GNU keyring when it
# was pinned), and built out of tree in ~/m68k/src. Cross-configured by
# ports/cross.sh, with config.cache giving the answers configure cannot
# find out without running something on the machine.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=5.3
SHA256=0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba
URL=https://ftp.gnu.org/gnu/bash/bash-$VERSION.tar.gz
SRC=$SRCDIR/bash-$VERSION
BUILD=$SRCDIR/build-bash-sage040

# GNU's own bug-fix patches for 5.3, as every distribution applies them,
# each pinned by its SHA-256 (their GPG signatures were checked against
# the GNU keyring when they were pinned). bash reports itself as 5.3.N.
PATCHES_URL=https://ftp.gnu.org/gnu/bash/bash-$VERSION-patches
PATCHES="
    bash53-001 1f608434364af86b9b45c8b0ea3fb3b165fb830d27697e6cdfc7ac17dee3287f
    bash53-002 e385548a00130765ec7938a56fbdca52447ab41fabc95a25f19ade527e282001
    bash53-003 f245d9c7dc3f5a20d84b53d249334747940936f09dc97e1dcb89fc3ab37d60ed
    bash53-004 9591d245045529f32f0812f94180b9d9ce9023f5a765c039b852e5dfc99747d0
    bash53-005 cca1ef52dbbf433bc98e33269b64b2c814028efe2538be1e2c9a377da90bc99d
    bash53-006 29119addefed8eff91ae37fd51822c31780ee30d4a28376e96002706c995ff10
    bash53-007 c0976bbfffa1453c7cfdd62058f206a318568ff2d690f5d4fa048793fa3eb299
    bash53-008 097cd723cbfb8907674ac32214063a3fd85282657ec5b4e544d2c0f719653fb4
    bash53-009 eee30fe78a4b0cb2fe20e010e00308899cfc613e0774ebb3c8557a1552f24f8c
    bash53-010 cf76f1cce2ea300c18bff9f002d21f280cc931acd17c28518110b93fe6e72569
    bash53-011 0298df8f5ea2a31d3be43ed7d269c5b3c7c342dd5b570bea7f64d66dcbbe7531
    bash53-012 d71379b39bebaedaf123414414e77fb458a0a43b9ad3116594c6df7ca6754573
    bash53-013 042f9cda967e24bf4211944697441e93d06ff42b4b998629a98a1b249279f200
    bash53-014 bd4360b401d38507e358783dcad8536a99c6789f0d3a5bd0cfb8c4a34144696c
    bash53-015 55b79ceee2fc27f6767eed697e939a7eb2fe2a28c01556bd75f18d581014f46e
    bash53-016 9ea29b266b7d24cb34d0ff3f1c4631e4d527bfe2d1ef15d17cdb924bf31ef767
    bash53-017 443b927b45c1558ca72052410f8b8f6e5152b617ed707061a2781d4375b0d1c3
    bash53-018 ae715d76c50341d7d7095e9a8d2eeed1ca9546152c2ac7289206f90cf30ac697
    bash53-019 a25c581e4d0057dea3833918438a930e2e86ee4c6dc17fe15267b7f04cbc4e3d
    bash53-020 df217ed3a9122aa2286d9b67bbe348661b6a9db262b580c29150dae55d532896
"

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR" "$SRCDIR/bash-$VERSION-patches"
    tarball=$SRCDIR/bash-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
    echo "$PATCHES" | while read -r name sum; do
        [ -n "$name" ] || continue
        f=$SRCDIR/bash-$VERSION-patches/$name
        [ -f "$f" ] || curl -sL --fail -o "$f" "$PATCHES_URL/$name"
        echo "$sum  $f" | sha256sum -c --quiet -
        patch -d "$SRC" -p0 -s < "$f"
    done
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# readline defines termcap's PC, UP and BC itself unless told the
# termcap library does -- and libc/termcap does.
BASH_CPPFLAGS="$CROSS_CPPFLAGS -DNEED_EXTERN_PC"

libc_fresh "$BUILD" || true   # reconfigured if picolibc's headers changed
if [ ! -f "$BUILD/Makefile" ]; then
    cp "$HERE/config.cache" "$BUILD/config.cache"
    (cd "$BUILD" && "$SRC/configure" --cache-file=config.cache \
        --host="$HOST_TRIPLET" --build="$(sh "$SRC/support/config.guess")" \
        CC="$CROSS_CC" AR="$CROSS_BIN/m68k-elf-ar" RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        CPPFLAGS="$BASH_CPPFLAGS" CFLAGS="$CROSS_CFLAGS" \
        LDFLAGS="$STATIC_LDFLAGS" LIBS="$STATIC_LIBS" \
        --without-bash-malloc --disable-nls > configure.log)
    # configure's link tests were static; bash itself is linked against
    # /lib/libc.so. bash's LIBS also names its own libraries, so the
    # system part is swapped in the generated Makefiles rather than
    # overridden on make's command line.
    python3 - "$STATIC_LIBS" "$DYN_LIBS" "$STATIC_LDFLAGS" "$DYN_LDFLAGS" "$BUILD" <<'PY'
import sys, glob, os
sl, dl, sf, df, b = sys.argv[1:6]
for p in glob.glob(os.path.join(b, '**', 'Makefile'), recursive=True):
    s = open(p).read()
    t = s.replace(sl, dl).replace(sf, df)
    if t != s:
        open(p, 'w').write(t)
PY
fi
make -C "$BUILD" -j8
cp "$BUILD/bash" "$HERE/bash"
"$CROSS_BIN/m68k-elf-size" "$HERE/bash"
