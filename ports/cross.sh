# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# cross.sh - sourced by a port's build.sh: the variables a GNU-style
# `./configure --host=... && make` needs to build for this machine.
#
# configure's link tests are static links, which need nothing on the
# machine; the program itself is linked against /lib/libc.so by passing
# DYN_LDFLAGS/DYN_LIBS to make instead (`make LDFLAGS=... LIBS=...`).
#
# --host=m68k-unknown-elf, not m68k-linux-gnu: gnulib's cross-compiling
# guesses assume glibc for a *-linux-gnu host and skip replacing what
# glibc gets right and picolibc may not. An unknown host gets its
# conservative guesses, and replacements where it is unsure.

TOP=${TOP:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

CROSS_CC=$M68K_PREFIX/bin/m68k-elf-gcc
[ -x "$CROSS_CC" ] || CROSS_CC=$(command -v m68k-elf-gcc)
[ -f "$SAGE_LIBC/lib/libc.so" ] || {
    echo "cross.sh: no picolibc in $SAGE_LIBC -- run 'make libc'" >&2
    exit 1
}
CROSS_BIN=$(dirname "$CROSS_CC")

HOST_TRIPLET=m68k-unknown-elf
# The include paths in CPPFLAGS, not CFLAGS: configure's preprocessor-only
# tests (AC_EGREP_CPP, AC_PREPROC_IFELSE) use CPPFLAGS alone, and with the
# paths only in CFLAGS they read the compiler's bare headers instead of
# picolibc's -- gnulib then decided PATH_MAX did not exist, and more.
CROSS_CPPFLAGS="-nostdinc -isystem $SAGE_LIBC/include \
-isystem $("$CROSS_CC" -print-file-name=include) -D_GNU_SOURCE"
CROSS_CFLAGS="-mcpu=68040 -O2"
STATIC_LDFLAGS="-nostdlib -Wl,-Bstatic -T $TOP/libc/sage040.ld \
-Wl,--build-id=none -Wl,--no-warn-rwx-segments $TOP/libc/crt0.s \
-L$SAGE_LIBC/lib"
STATIC_LIBS="-Wl,--start-group -lc -llinux -Wl,--end-group -lgcc"
DYN_LDFLAGS="-nostdlib -Wl,-Ttext-segment=0x10000000 \
-Wl,--dynamic-linker=/lib/ld.so -Wl,-z,now -Wl,--hash-style=sysv \
-Wl,--build-id=none $TOP/libc/crt0-dyn.s -L$SAGE_LIBC/lib"
DYN_LIBS="-lc -lgcc"

# THE SPECS FILE, which is the other way to link for this machine.
#
# Everything above hands configure a link line spelled out in full.
# That works while a Makefile in this tree does the linking, and stops
# working the moment something else does: binutils' top-level configure
# checks the compiler with
#
#     ${CC} -o conftest ${CFLAGS} ${CPPFLAGS} ${LDFLAGS} conftest.c
#
# and no ${LIBS} anywhere, so a link line that depends on LIBS cannot
# pass it. libc/sage040.specs puts the knowledge in the compiler
# instead: `gcc hello.c -o hello` links a dynamic program, `gcc -static`
# a static one, and nothing else has to be said. It is also exactly what
# the NATIVE compiler needs, since somebody typing at a prompt on the
# machine will say no more than that either.
#
# -B, not -L, for the start files and the linker script: gcc looks for
# %s files in the startfile prefixes, and -L directories are not among
# them.
SAGE_SPECS=$TOP/libc/sage040.specs
SPECS_CFLAGS="-specs=$SAGE_SPECS -B$TOP/libc/ -B$SAGE_LIBC/lib/ -L$SAGE_LIBC/lib"

# The start files have to exist as OBJECTS for a spec to name one.
for _crt in crt0 crt0-dyn; do
    if [ ! -f "$TOP/libc/$_crt.o" ] ||        [ "$TOP/libc/$_crt.s" -nt "$TOP/libc/$_crt.o" ]; then
        "$CROSS_CC" -mcpu=68040 -c "$TOP/libc/$_crt.s" -o "$TOP/libc/$_crt.o"
    fi
done
unset _crt

# configure, cross: CC and friends, and the answers configure cannot
# find out without running a program on the machine.
cross_configure() {             # cross_configure SRCDIR [configure args...]
    local src=$1
    shift
    #
    # C++ IS POINTED AT THE C COMPILER, WITH THE C FRONT END FORCED.
    #
    # The cross toolchain has no g++ -- no cc1plus -- and several of
    # these configure scripts run AC_PROG_CXXCPP whether or not the
    # package has any C++ in it. With nothing said, autoconf falls back
    # to the HOST's /lib/cpp and then hands it our CPPFLAGS, which say
    # -nostdinc and point at picolibc; a host preprocessor with no host
    # headers fails the sanity check, and the build stops in a package
    # that never needed C++ at all. Naming the cross compiler is not
    # enough either: it refuses a .cpp input with "C++ compiler not
    # installed on this system". -x c makes it preprocess the probe as
    # C, which is all these tests actually require.
    #
    # Safe because nothing that uses cross_configure compiles C++:
    # libffi, less, grep, readline, sed, ncurses and xz. gcc and
    # libstdcxx, which do, set their own CXX and do not come through
    # here.
    "$src/configure" --host="$HOST_TRIPLET" --build="$("$src/build-aux/config.guess")" \
        CC="$CROSS_CC" \
        CXX="$CROSS_CC -x c" CXXCPP="$CROSS_CC -E -x c" \
        AR="$CROSS_BIN/m68k-elf-ar" RANLIB="$CROSS_BIN/m68k-elf-ranlib" \
        CPPFLAGS="$CROSS_CPPFLAGS" CFLAGS="$CROSS_CFLAGS" \
        LDFLAGS="$STATIC_LDFLAGS" LIBS="$STATIC_LIBS" \
        "$@"
}

# libc_fresh DIR: empty DIR if the C library's headers have changed since
# what is in it was built against them, and note the ones it is being
# built against now. Returns 1 if it emptied it. A header change can
# change a structure's size -- struct tm grew -- and an object built with
# the old one is silently wrong; nothing else here notices.
libc_fresh() {
    local sum
    sum=$(cat "$SAGE_LIBC/lib/.headers-sum" 2>/dev/null || echo none)
    if [ -d "$1" ] && [ "$(cat "$1/.libc-headers" 2>/dev/null)" = "$sum" ]; then
        return 0
    fi
    rm -rf "$1"
    mkdir -p "$1"
    echo "$sum" > "$1/.libc-headers"
    return 1
}


# ---------------------------------------------------------------------
# THE GCC SOURCE, AND THE SECOND CROSS COMPILER BUILT FROM IT
#
# Three things want gcc's source -- ports/gcc, ports/libstdcxx, and the
# cross g++ below -- so the version, the URL and the checksum are said
# once, here. They were about to be said in three files, which is how
# two of them end up on different versions and the third finds out.
# ---------------------------------------------------------------------
GCC_VERSION=15.2.0
GCC_SHA256=438fd996826b0c82485a29da03a72d71d6e3541a83ec702df4271f6fe025d24e
GCC_URL=https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz

# Unpack gcc's source if it is not already there; print where it is.
gcc_source() {
    local src=$SRCDIR/gcc-$GCC_VERSION
    if [ ! -d "$src" ]; then
        mkdir -p "$SRCDIR"
        local tarball=$SRCDIR/gcc-$GCC_VERSION.tar.xz
        [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$GCC_URL"
        echo "$GCC_SHA256  $tarball" | sha256sum -c - >&2
        tar -C "$SRCDIR" -xf "$tarball"
    fi
    echo "$src"
}

# The SECOND cross compiler: gcc for m68k with C++, in a prefix of its
# own. toolchain.md, "Why there is a second cross compiler", says why it
# has to exist: gcc 15 is written in C++, so a gcc that runs on the
# machine needs a libstdc++ for the machine, and building that needs a
# C++ compiler targeting m68k -- which the C-only cross toolchain in
# ~/m68k/install is not.
#
# It was a block of shell in that document, to be typed by hand. That
# made it the one step of the toolchain a fresh machine could not do,
# and `make install` failed there with a note telling the reader to go
# and read the document. Automating it is the difference between a tree
# that builds anywhere and one that builds where somebody once did this.
#
# A SEPARATE PREFIX ON PURPOSE: ~/m68k/install is the C compiler
# everything else here depends on, and adding a language to it is not
# worth the risk. Same version, same target, so their objects link.
cross_cxx() {
    local prefix=${SAGE_CXX:-$HOME/m68k/install-cxx}
    [ -x "$prefix/bin/m68k-elf-g++" ] && { echo "$prefix"; return 0; }

    local src build
    src=$(gcc_source)
    build=$SRCDIR/build-gcc-cxx
    echo "cross g++: building gcc $GCC_VERSION (c,c++) into $prefix" >&2
    echo "           once, and it takes a while." >&2
    mkdir -p "$build"
    if [ ! -f "$build/Makefile" ]; then
        # CXX="g++ -std=gnu++17" because a host g++ defaulting to C++20
        # -- GCC 16 does -- compiles gcc 15's own libcody wrongly: u8""
        # literals became char8_t in C++20 and libcody predates it.
        ( cd "$build" && PATH="$CROSS_BIN:$PATH" "$src/configure" \
            --target=m68k-elf --prefix="$prefix" \
            --disable-nls --enable-languages=c,c++ --without-headers \
            --with-gnu-as --with-gnu-ld --disable-multilib \
            --with-cpu=68040 \
            CXX="g++ -std=gnu++17" CXX_FOR_BUILD="g++ -std=gnu++17" \
            > configure.log 2>&1 ) \
            || { tail -30 "$build/configure.log" >&2; return 1; }
    fi
    # all-gcc/install-gcc: the compiler and NOT the target libgcc, which
    # is why this prefix has no libgcc.a and the ports -B at the C
    # toolchain's instead. Same version, same target, and libgcc does
    # not depend on which front ends were built.
    ( cd "$build" && PATH="$CROSS_BIN:$PATH" \
        make -j"$(nproc)" all-gcc > make.log 2>&1 \
        && make install-gcc >> make.log 2>&1 ) \
        || { tail -30 "$build/make.log" >&2; return 1; }
    echo "$prefix"
}
