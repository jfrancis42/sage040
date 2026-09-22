#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - ncurses for SuckOS: the terminfo database and curses.
#
# Two builds, because ncurses needs its own tools to make its database:
#
#   1. A HOST build, whose `tic` compiles terminfo sources into the
#      binary database. Cross-compiling ncurses without one leaves
#      nothing to read.
#   2. The CROSS build: libncurses, libtinfo (terminfo alone -- what
#      `less` and a shell want), libform, libmenu, libpanel, and the
#      programs tput, tset, infocmp, clear and tabs.
#
# The database goes on the machine's disk at /usr/share/terminfo, laid
# out as ncurses lays it out -- one directory per first letter, one file
# per terminal. That is a real database read at run time, not a terminal
# compiled into the library.
#
# NOTHING BUILT LANDS IN THIS DIRECTORY. The libraries, the programs and
# the database are left in the build tree under ~/m68k/src and installed
# onto the disk from there, because this one is in Dropbox and under
# git: a terminfo tree has a directory per first letter, upper and lower
# case both, and a case-insensitive filesystem -- Dropbox's, and FAT's
# -- cannot hold `A` and `a` side by side. The first attempt put it here
# and came back with directories named "a (Case Conflict)".

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=6.5
SHA256=136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6
URL=https://ftp.gnu.org/gnu/ncurses/ncurses-$VERSION.tar.gz
SRC=$SRCDIR/ncurses-$VERSION
BUILD=$SRCDIR/build-ncurses-sage040
BUILD_HOST=$SRCDIR/build-ncurses-host

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/ncurses-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
fi

# --- 1. the host build, for tic ---------------------------------------
#
# Only far enough to have the tools: `make -C progs tic` after the
# headers and libtinfo it needs.
if [ ! -x "$BUILD_HOST/progs/tic" ]; then
    mkdir -p "$BUILD_HOST"
    (cd "$BUILD_HOST" && "$SRC/configure" \
        --prefix="$BUILD_HOST/staging" \
        --without-shared --without-debug --without-ada --without-cxx \
        --without-cxx-binding --without-manpages --without-tests \
        --disable-db-install \
        > configure.log 2>&1)
    make -C "$BUILD_HOST" -j8 > "$BUILD_HOST/make.log" 2>&1
fi

TIC=$BUILD_HOST/progs/tic
[ -x "$TIC" ] || { echo "ncurses: the host tic did not build" >&2; exit 1; }

# --- 2. the cross build ------------------------------------------------
#
# What each option is for:
#   --without-cxx-binding   no C++ on this machine
#   --disable-stripping     the cross strip, not the host's
#   --with-termlib          libtinfo separately: `less` wants terminfo,
#                           not curses, and so does anything that only
#                           asks how to clear the screen
#   --enable-termcap        the termcap entry points too, over terminfo,
#                           so a program written against <termcap.h>
#                           links either way
#   --disable-home-terminfo no $HOME/.terminfo: one database, on the disk
#   --with-fallbacks        vt102 and vt100 compiled in, so a program
#                           works before the database is installed and
#                           on a disk that has none
#   --disable-db-install    the cross build must not try to run its own
#                           tic; the database is made below with the
#                           host's
libc_fresh "$BUILD" || true
if [ ! -f "$BUILD/Makefile" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && cross_configure "$SRC" \
        --prefix=/usr \
        --datadir=/usr/share \
        --without-shared --without-debug --without-ada --without-cxx \
        --without-cxx-binding --without-manpages --without-tests \
        --with-termlib --enable-termcap --disable-home-terminfo \
        --disable-db-install \
        --with-fallbacks=vt102,vt100,dumb,unknown \
        --with-build-cc="${BUILD_CC:-cc}" \
        --disable-stripping \
        ac_cv_func_getttynam=no \
        cf_cv_func_nanosleep=yes \
        > configure.log 2>&1)
fi
make -C "$BUILD" -j8 libs > "$BUILD/make.log" 2>&1

# The programs, linked against /lib/libc.so rather than statically: the
# flags go on make's command line, as they do for every other port here.
make -C "$BUILD/progs" -j8 \
    LDFLAGS="$DYN_LDFLAGS" LIBS="$DYN_LIBS" \
    tput tset infocmp clear tabs >> "$BUILD/make.log" 2>&1 || true

# ncurses 6 builds the WIDE library by default and names it libncursesw;
# a program that says -lncurses wants that one, so each is installed
# under both names. The tree is what a program links against, so
# <curses.h> and friends come too.
OUT=$BUILD/sage040
mkdir -p "$OUT/lib" "$OUT/bin" "$OUT/include"
for l in ncurses tinfo form menu panel; do
    if [ -f "$BUILD/lib/lib${l}w.a" ]; then
        cp "$BUILD/lib/lib${l}w.a" "$OUT/lib/"
        cp "$BUILD/lib/lib${l}w.a" "$OUT/lib/lib$l.a"
    fi
done
cp "$BUILD/include/"*.h "$OUT/include/" 2>/dev/null || true
cp "$SRC/include/"*.h "$OUT/include/" 2>/dev/null || true
for p in tput tset infocmp clear tabs; do
    [ -x "$BUILD/progs/$p" ] && cp "$BUILD/progs/$p" "$OUT/bin/$p"
done

# --- 3. the terminfo database -----------------------------------------
#
# Compiled with the HOST's tic into a tree here, which `make install`
# copies onto the disk. TERMINFO_TERMS limits it: the whole of
# terminfo.src is about 2,500 terminals and 40,000 files, which is a
# great deal of FAT directory for a machine that will ever see four of
# them. The default set is everything this machine can actually be.
TERMS=${TERMINFO_TERMS:-vt102 vt100 vt220 vt52 ansi xterm xterm-256color \
    linux screen tmux dumb unknown sun wyse50 vt320 rxvt putty}

rm -rf "$OUT/terminfo"
mkdir -p "$OUT/terminfo"
TERMINFO="$OUT/terminfo" "$TIC" -x -s -o "$OUT/terminfo" \
    "$SRC/misc/terminfo.src" > "$BUILD/tic.log" 2>&1 || true

# Keep only the wanted terminals: tic compiles the lot in one pass, and
# pruning afterwards is simpler than feeding it a filtered source.
#
# The pruning is not only about size. The machine's filesystem is FAT,
# which is CASE-INSENSITIVE: the full database has entries whose first
# letters differ only in case (Eterm and emu), and their directories
# cannot both exist there. Every terminal in the list below begins with
# a lower-case letter for that reason.
if [ "${TERMINFO_ALL:-0}" != 1 ]; then
    keep=$BUILD/.keep
    : > "$keep"
    for t in $TERMS; do
        printf '%s/%s\n' "$(printf '%s' "$t" | cut -c1)" "$t" >> "$keep"
    done
    (cd "$OUT/terminfo" && find . -type f | sed 's|^\./||' |
        while read -r f; do
            grep -qxF "$f" "$keep" || rm -f "$f"
        done
     find . -type d -empty -delete)
    rm -f "$keep"
fi

# No two of them may differ only in case, or the disk cannot hold both.
dupes=$(cd "$OUT/terminfo" && find . -type f | sed 's|.*/||' |
        tr 'A-Z' 'a-z' | sort | uniq -d)
if [ -n "$dupes" ]; then
    echo "ncurses: these differ only in case, which FAT cannot hold:" >&2
    echo "$dupes" >&2
    exit 1
fi

echo "ncurses $VERSION -> $OUT"
echo "  $(find "$OUT/terminfo" -type f | wc -l) terminals, $(du -sk "$OUT/terminfo" | cut -f1) KB"
"$CROSS_BIN/m68k-elf-size" "$OUT"/lib/libncursesw.a 2>/dev/null | tail -n +2 |
    awk '{ t += $1 } END { printf "  libncurses: %d bytes of text\n", t }'
