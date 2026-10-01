#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - musl, as a second C library, for the few programs that
# need the kernel's interface as Linux defines it.
#
# picolibc is this system's C library and stays so. But it numbers
# things its own way -- signals, clocks, AT_ and SIGEV_ constants, the
# layout of siginfo -- and translates at the system call boundary, which
# is invisible to an ordinary program and fatal to one whose job is to
# describe that boundary: strace decodes the kernel's raw arguments, and
# with picolibc's constants it would decode them wrongly or not compile.
# musl's constants are Linux's, and its system calls are Linux/m68k's,
# which are this kernel's.
#
# Static only, and only the programs that ask for it are linked with it:
# a musl program carries its C library inside itself and shares nothing
# with the rest of the system.
#
# Two things this machine does differently from Linux are dealt with
# here rather than in musl:
#
#   the initial stack   this kernel passes argc and POINTERS to argv and
#                       envp; musl's crt1 wants Linux's contiguous block.
#                       sage040-crt1.c builds it (see there).
#   the layout          linked as the dynamic programs are, from 0x10000000
#                       with ld's own script, so that the program headers
#                       are in a loaded segment: musl finds its thread-
#                       local storage through them (AT_PHDR), and
#                       libc/sage040.ld does not load them.
#
# The result is $OUT, with musl's headers and libraries, the start file,
# and bin/m68k-sage040-musl-gcc, a compiler that builds against it.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=1.2.6
SHA256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a
URL=https://musl.libc.org/releases/musl-$VERSION.tar.gz
SRC=$SRCDIR/musl-$VERSION
BUILD=$SRCDIR/build-musl-sage040
OUT=$BUILD/sage040

"$HERE/../linux-headers/build.sh" > /dev/null
KHDR=$SRCDIR/build-linux-headers-sage040/sage040/include

if [ ! -d "$SRC" ]; then
    mkdir -p "$SRCDIR"
    tarball=$SRCDIR/musl-$VERSION.tar.gz
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$URL"
    echo "$SHA256  $tarball" | sha256sum -c -
    tar -C "$SRCDIR" -xzf "$tarball"
fi

if [ ! -f "$BUILD/config.mak" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" --target=m68k --prefix="$OUT" --disable-shared \
        CC="$CROSS_CC -mcpu=68040" CROSS_COMPILE="$CROSS_BIN/m68k-elf-" \
        > configure.log 2>&1) || { tail -20 "$BUILD/configure.log"; exit 1; }
fi
make -C "$BUILD" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { grep -E 'error' "$BUILD/make.log" | head; exit 1; }
make -C "$BUILD" install > "$BUILD/install.log" 2>&1

"$CROSS_CC" -mcpu=68040 -O2 -c "$HERE/sage040-crt1.c" -o "$OUT/lib/sage040-crt1.o"

mkdir -p "$OUT/bin"
cat > "$OUT/bin/m68k-sage040-musl-gcc" <<WRAP
#!/bin/sh
# Compile and link against musl, for this machine (ports/musl).
link=1
for a in "\$@"; do
    case "\$a" in -c|-S|-E|-M|-MM) link=0 ;; esac
done
inc="-nostdinc -isystem $OUT/include -isystem $KHDR -isystem $("$CROSS_CC" -print-file-name=include)"
if [ \$link = 0 ]; then
    exec "$CROSS_CC" -mcpu=68040 \$inc "\$@"
fi
exec "$CROSS_CC" -mcpu=68040 \$inc -nostdlib -Wl,-Bstatic \\
    -Wl,-Ttext-segment=0x10000000 -Wl,--build-id=none \\
    $OUT/lib/sage040-crt1.o $OUT/lib/crti.o "\$@" \\
    -L$OUT/lib -Wl,--start-group -lc -lgcc -Wl,--end-group $OUT/lib/crtn.o
WRAP
chmod +x "$OUT/bin/m68k-sage040-musl-gcc"
echo "musl $VERSION -> $OUT"
