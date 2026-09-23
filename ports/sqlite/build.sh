#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - SQLite for SuckOS: the library, and the shell.
#
# CPython's sqlite3 module wants the library; the shell is a database
# somebody can actually use from the prompt.
#
# The amalgamation, not the source tree: one C file, no configure worth
# the name, and the only build SQLite's authors test against.
#
# WHAT IT NEEDS FROM THIS SYSTEM, and what is switched off because it
# does not have it: no file locking beyond flock (SQLITE_ENABLE_LOCKING
# is left alone -- FAT has no byte-range locks, and one process at a
# time is what this machine does), and no memory mapping of database
# files (mmap of a FAT file is a copy here, so it would be slower and
# wronger than a read).

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=3530400                 # 3.53.4
#
# SHA3-256, NOT sha256, because that is what SQLite publishes -- the
# machine-readable product line in download.html carries it, and it is
# the only hash the authors state anywhere. Checking a sha256 of my own
# computing would only prove the file had not changed since I fetched
# it, which is not the question.
SHA3=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
URL=https://www.sqlite.org/2026/sqlite-amalgamation-$VERSION.zip
SRC=$SRCDIR/sqlite-amalgamation-$VERSION
BUILD=$SRCDIR/build-sqlite-sage040

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    zip=$SRCDIR/sqlite-amalgamation-$VERSION.zip
    [ -f "$zip" ] || curl -L --fail -o "$zip" "$URL"
    got=$(openssl dgst -sha3-256 "$zip" | awk '{print $NF}')
    [ "$got" = "$SHA3" ] || { echo "sqlite: sha3-256 mismatch: $got" >&2; exit 1; }
    (cd "$SRCDIR" && unzip -q -o "$zip")
fi

libc_fresh "$BUILD" || true
mkdir -p "$BUILD"

# What to leave out, and why: no dynamic extension loading (there is no
# dlopen), no shared cache (one process), and the threadsafe mode that
# needs recursive mutexes is fine -- this system has them.
SQL_FLAGS="-DSQLITE_OMIT_LOAD_EXTENSION=1 \
-DSQLITE_THREADSAFE=1 \
-DSQLITE_ENABLE_COLUMN_METADATA=1 \
-DSQLITE_ENABLE_FTS5=1 \
-DSQLITE_ENABLE_RTREE=1 \
-DSQLITE_ENABLE_MATH_FUNCTIONS=1 \
-DSQLITE_DEFAULT_MMAP_SIZE=0 \
-DSQLITE_MAX_MMAP_SIZE=0 \
-DHAVE_USLEEP=1 -DHAVE_FDATASYNC=1 -DHAVE_LOCALTIME_R=1 -DHAVE_STRERROR_R=1"

# shellcheck disable=SC2086
"$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS $SQL_FLAGS \
    -c "$SRC/sqlite3.c" -o "$BUILD/sqlite3.o" 2> "$BUILD/build.log"
"$CROSS_BIN/m68k-elf-ar" rcs "$BUILD/libsqlite3.a" "$BUILD/sqlite3.o"
"$CROSS_BIN/m68k-elf-ranlib" "$BUILD/libsqlite3.a"

# The shell, against /lib/libc.so.
# shellcheck disable=SC2086
"$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS $SQL_FLAGS -I"$SRC" \
    $DYN_LDFLAGS "$SRC/shell.c" "$BUILD/libsqlite3.a" $DYN_LIBS \
    -o "$BUILD/sqlite3" 2>> "$BUILD/build.log"

OUT=$BUILD/sage040
mkdir -p "$OUT/lib" "$OUT/include" "$OUT/bin"
cp "$BUILD/libsqlite3.a" "$OUT/lib/"
cp "$SRC/sqlite3.h" "$SRC/sqlite3ext.h" "$OUT/include/"
cp "$BUILD/sqlite3" "$OUT/bin/sqlite3"

echo "sqlite $VERSION -> $OUT"
"$CROSS_BIN/m68k-elf-size" "$OUT/bin/sqlite3" | tail -1
