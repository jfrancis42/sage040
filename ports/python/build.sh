#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - CPython for SuckOS.
#
# The largest port here by a wide margin, and a cross build of CPython
# has three things in it that no other port here does:
#
# Two of its options are chosen for this machine rather than by taste:
# --without-mimalloc, because mimalloc reaches for compiler
# thread-local storage, which needs a thread register m68k has not got
# (pymalloc, CPython's older allocator, is used instead); and
# --disable-ipv6, because the network stack is IPv4.
#
#   1. IT NEEDS A PYTHON TO BUILD PYTHON. The build runs the interpreter
#      on the host -- to freeze the startup modules, to build the
#      standard library's .pyc files, to run setup.py. That has to be
#      the SAME VERSION as what is being built, which is why the version
#      below is pinned to the host's.
#   2. EVERY MODULE IS BUILT IN. There is no dlopen here, so
#      Modules/Setup.local names the extension modules statically and
#      the build makes one executable with the lot inside it.
#   3. The standard library is FILES, and they go on the disk. Python
#      finds them through its prefix, /usr/local, so sys.path works
#      without anything being told where anything is.
#
# Nothing built lands in this directory: it all goes to the build tree
# under ~/m68k/src, and `make install` copies it onto the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=3.14.7
SHA256=3b48dac8fb59f62eaa67ac83c1eb12bda1b7a08406dd286e252c11a66be27f81
URL=https://www.python.org/ftp/python/$VERSION/Python-$VERSION.tar.xz
SRC=$SRCDIR/Python-$VERSION
BUILD=$SRCDIR/build-python-sage040
OUT=$BUILD/sage040

# The host interpreter that builds the target one. It must be the same
# version, to the minor number: the build runs it over the target's own
# source and unpickles what it writes.
BUILD_PYTHON=${BUILD_PYTHON:-$(command -v python3)}
host_ver=$("$BUILD_PYTHON" -c 'import sys; print("%d.%d" % sys.version_info[:2])')
want_ver=${VERSION%.*}
if [ "$host_ver" != "$want_ver" ]; then
    echo "python: the build interpreter is $host_ver and the target is $want_ver." >&2
    echo "        Set BUILD_PYTHON to a $want_ver, or change VERSION here." >&2
    exit 1
fi

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/Python-$VERSION.tar.xz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# PKG_CONFIG=/bin/false, and the curses flags given outright.
#
# configure asks pkg-config what libraries exist, and pkg-config answers
# about the HOST's -- it said ncursesw was available and set the link
# flags from the host's package, which is how a cross build ends up
# linking against a library for the wrong machine, or, here, missing
# the libtinfow half and failing at the last link with undefined
# references to ncurses's own symbols. A cross build must not ask the
# host what it has.
#
# The two libraries name each other, so ncursesw appears twice: a static
# link scans each archive once, in order.

# The modules to build in. Everything Python needs to start, plus what
# makes it useful here: the maths, the hashes, sockets, select, time,
# the terminal, and curses over the ncurses built by ports/ncurses.
NCOUT=$SRCDIR/build-ncurses-sage040/sage040
ZOUT=$SRCDIR/build-zlib-sage040/sage040

# The libraries Python links against, each built by another port here.
if [ ! -f "$ZOUT/lib/libz.a" ]; then
    "$HERE/../zlib/build.sh"
fi
if [ ! -f "$NCOUT/lib/libncursesw.a" ]; then
    "$HERE/../ncurses/build.sh"
fi

#
# WHY uint32_t IS NOT unsigned int HERE, and why that is allowed to
# pass. This bare-metal m68k-elf compiler makes int32_t a LONG, where
# Linux/m68k's C library makes it an INT. Both are four bytes, both are
# two-byte aligned (__BIGGEST_ALIGNMENT__ is 2 on this machine), and
# they have the same representation -- so the only difference is the
# name of the type, and every pointer between them is
# interchangeable in fact while being a different type in law.
#
# CPython passes `unsigned int *` to a function taking `uint32_t *` in
# a handful of places. GCC 14 made that an error rather than a warning.
# Patching CPython would mean changing its declarations to suit one
# toolchain's arbitrary choice; allowing the conversion here is honest
# about what it is, and is safe for exactly the reason above.
#
PY_CFLAGS="-Wno-incompatible-pointer-types"

libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    mkdir -p "$BUILD/Modules"
    cp "$HERE/Setup.local" "$BUILD/Modules/Setup.local"

    # config.cache: the answers configure would need to run a program on
    # the machine to find out, each one a fact about this system.
    cp "$HERE/config.cache" "$BUILD/config.cache"

    # m68k-unknown-LINUX-gnu, where every other port here says
    # m68k-unknown-elf. CPython's configure knows a fixed list of hosts
    # and refuses anything else outright ("cross build not supported"),
    # and of the ones it knows, linux is the true answer: this kernel's
    # system call numbers, calling convention and errnos are Linux's.
    # What it must NOT do is make the compiler claim to be Linux --
    # __linux__ stays undefined, so code that tests for it directly
    # (epoll, sendfile, /proc) compiles out, and config.cache says no to
    # each of those anyway.
    # MODULE_BUILDTYPE=static: every extension module is linked INTO the
    # interpreter. configure defaults to shared, which needs a dlopen
    # this system does not have -- and a shared module here links
    # against nothing, so it fails with several hundred undefined
    # references to the interpreter's own symbols.
    (cd "$BUILD" && MODULE_BUILDTYPE=static "$SRC/configure" \
        --cache-file=config.cache \
        --host=m68k-unknown-linux-gnu \
        --build="$("$SRC/config.guess")" \
        --prefix=/usr/local \
        --with-build-python="$BUILD_PYTHON" \
        --disable-shared \
        --without-static-libpython \
        --disable-ipv6 \
        --without-mimalloc \
        --with-ensurepip=no \
        --without-doc-strings \
        --disable-test-modules \
        CC="$CROSS_CC" \
        AR="$CROSS_BIN/m68k-elf-ar" RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        PKG_CONFIG=/bin/false \
        CURSES_CFLAGS="-I$NCOUT/include" \
        CURSES_LIBS="-lncursesw -ltinfow -lncursesw" \
        PANEL_CFLAGS="-I$NCOUT/include" \
        PANEL_LIBS="-lpanelw -lncursesw -ltinfow -lncursesw" \
        READELF="$CROSS_BIN/m68k-elf-readelf" \
        CPPFLAGS="$CROSS_CPPFLAGS -I$NCOUT/include -I$ZOUT/include" \
        CFLAGS="$CROSS_CFLAGS $PY_CFLAGS" \
        LDFLAGS="$STATIC_LDFLAGS -L$NCOUT/lib -L$ZOUT/lib" \
        LIBS="$STATIC_LIBS" \
        > configure.log 2>&1) || { tail -40 "$BUILD/configure.log"; exit 1; }
fi

# SYSLIBS goes LAST on every link, after Python's own archives; LIBS
# goes before them. The C library and libgcc have to be in the last one:
# a static link scans each archive once, in order, so anything the
# archives in between need -- libexpat's __floatundisf, ncurses's
# wcwidth and wcrtomb -- is undefined if libc and libgcc were scanned
# before the archive that asks for them.
make -C "$BUILD" -j8 SYSLIBS="$STATIC_LIBS" "$@" 2>&1 | tail -40

mkdir -p "$OUT"
make -C "$BUILD" install DESTDIR="$OUT" > "$BUILD/install.log" 2>&1 || true

echo "python $VERSION -> $OUT"
[ -x "$BUILD/python" ] && "$CROSS_BIN/m68k-elf-size" "$BUILD/python"
