#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - git for SuckOS.
#
# git's Makefile cross-builds without configure: config.mak, written
# below, says what the target is (Linux, m68k) and what it has. What it
# is built with, and without:
#
#   with     zlib, OpenSSL, libcurl (git-remote-https, through all of
#            curl's own dependencies, as ports/curl links them), libiconv
#            (commit encodings), threads, and the Perl commands --
#            send-email, svn, cvsimport -- against /usr/bin/perl
#   without  Rust (2.56 builds Rust by default; there is no Rust for
#            m68k), Python, Tcl/Tk, expat (only push over WebDAV wants
#            it) and gettext (no translations installed anyway)
#
#   HAVE_DEV_TTY is NOT set, though Linux's defaults set it: there is no
#   /dev/tty on this system yet (todo), and git would open it to prompt
#   for credentials and fail.
#   HAVE_SYSINFO is not set either: picolibc has no <sys/sysinfo.h>,
#   and git asks it only for the size of memory, with a fallback.
#   HAVE_SYNC_FILE_RANGE, likewise: no sync_file_range here; git falls
#   back to fsync, which there is.
#   No fsmonitor daemon: its Linux backend wants <sys/statfs.h>, and it
#   is an optional accelerator for very large worktrees.
#   NO_REGEX: git's own regex, not picolibc's, which git's needs
#   (REG_STARTEND) and nothing here has tested.
#
# Built in the unpacked source tree, which is the build tree. Nothing
# built lands in this directory: `make install` copies it onto the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=2.56.0
SHA256=26c56c296b38c0695b26fa95f475f1d01704d2d38e73465ca30b0b2f5dc789d3
URL=https://mirrors.edge.kernel.org/pub/software/scm/git/git-$VERSION.tar.xz
BUILD=$SRCDIR/build-git-sage040
SRC=$BUILD/git-$VERSION
OUT=$BUILD/sage040

for p in curl libiconv; do
    "$HERE/../$p/build.sh" > /dev/null || { echo "git: ports/$p/build.sh failed" >&2; exit 1; }
done
SSLOUT=$SRCDIR/build-openssl-sage040/sage040
ZOUT=$SRCDIR/build-zlib-sage040/sage040
CURLOUT=$SRCDIR/build-curl-sage040/sage040
ICONVOUT=$SRCDIR/build-libiconv-sage040/sage040
# libidn2.a WITHOUT gnulib's error(). The archive carries it (wget met
# the same object: ports/wget), nothing in libidn2 calls it, and git has
# an error() of its own: linking both is "multiple definition of `error'".
# A copy with that one object taken out is what git links.
mkdir -p "$BUILD/idn2"
cp "$SRCDIR/build-libidn2-sage040/sage040/lib/libidn2.a" "$BUILD/idn2/libidn2.a"
"$CROSS_BIN/m68k-elf-ar" d "$BUILD/idn2/libidn2.a" libgnu_la-error.o
# libcurl.a's own needs, in the order curl's Makefile links them.
CURLLIBS="-L$CURLOUT/lib -lcurl \
-L$SRCDIR/build-nghttp2-sage040/sage040/lib -lnghttp2 \
-L$BUILD/idn2 -lidn2 \
-L$SRCDIR/build-libpsl-sage040/sage040/lib -lpsl \
-L$SSLOUT/lib -lssl -lcrypto \
-L$SRCDIR/build-zstd-sage040/sage040/lib -lzstd \
-L$SRCDIR/build-brotli-sage040/sage040/lib -lbrotlidec -lbrotlicommon \
-L$ZOUT/lib -lz \
-L$SRCDIR/build-libunistring-sage040/sage040/lib -lunistring"

libc_fresh "$BUILD" || true
if [ ! -d "$SRC" ]; then
    tarball=$SRCDIR/git-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    mkdir -p "$BUILD"
    tar -C "$BUILD" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

cat > "$SRC/config.mak" <<MAK
uname_S = Linux
uname_M = m68k
uname_O = GNU/Linux
uname_R = 6.0
uname_P = unknown
uname_V = 1
CC = $CROSS_CC -mcpu=68040 $SPECS_CFLAGS
CFLAGS = -O2 $CROSS_CPPFLAGS
AR = $CROSS_BIN/m68k-elf-ar
STRIP = $CROSS_BIN/m68k-elf-strip
prefix = /usr
gitexecdir = /usr/libexec/git-core
PERL_PATH = /usr/bin/perl
SHELL_PATH = /bin/sh
NO_RUST = YesPlease
NO_PYTHON = YesPlease
NO_TCLTK = YesPlease
NO_EXPAT = YesPlease
NO_GETTEXT = YesPlease
NO_REGEX = NeedsStartEnd
HAVE_DEV_TTY =
HAVE_SYSINFO =
HAVE_SYNC_FILE_RANGE =
FSMONITOR_DAEMON_BACKEND =
FSMONITOR_OS_SETTINGS =
NEEDS_LIBICONV = YesPlease
ICONVDIR = $ICONVOUT
ZLIB_PATH = $ZOUT
OPENSSLDIR = $SSLOUT
CURLDIR = $CURLOUT
CURL_LDFLAGS = $CURLLIBS
INSTALL_SYMLINKS = YesPlease
SKIP_DASHED_BUILT_INS = YesPlease
NO_INSTALL_HARDLINKS = YesPlease
MAK

make -C "$SRC" -j"$(nproc)" all > "$BUILD/make.log" 2>&1 \
    || { grep -E 'error|Error' "$BUILD/make.log" | head -20; exit 1; }

rm -rf "$BUILD/inst"
make -C "$SRC" install DESTDIR="$BUILD/inst" > "$BUILD/install.log" 2>&1 \
    || { tail -30 "$BUILD/install.log"; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT"
cp -a "$BUILD/inst/usr/." "$OUT/"
rm -rf "$OUT/share/man" "$OUT/share/doc" "$OUT/share/gitweb" "$OUT/share/git-gui"
find "$OUT/bin" "$OUT/libexec" -type f | while read -r f; do
    case "$(head -c 4 "$f" | od -An -tx1 | tr -d ' ')" in
        7f454c46) "$CROSS_BIN/m68k-elf-strip" "$f" ;;
    esac
done

echo "git $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/git" | tail -1
du -sh "$OUT" | awk '{print "   total", $1}'
