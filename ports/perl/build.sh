#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# build.sh - Perl 5 for SuckOS.
#
# Perl's own Configure cannot cross-compile: it answers its questions by
# running test programs, and a 68040 program does not run on this
# workstation. perl-cross (github.com/arsv/perl-cross) replaces it with
# a configure that answers them by compiling only, and builds a host
# miniperl to run the build's Perl scripts. Buildroot and OpenWrt build
# Perl the same way. The perl-cross release has to name the Perl
# release: 1.6.5 is the first that knows 5.44.0.
#
# The result is /usr/bin/perl and its library under /usr/lib/perl5,
# with the XS extensions -- POSIX, Socket, Fcntl, List::Util and the
# rest -- as shared objects that DynaLoader dlopen()s, which is how
# every Linux Perl does it and what a CPAN module built on the machine
# will need. perl is linked -Wl,-E so that those objects find the
# interpreter's own symbols in it.
#
# 64-bit integers (-Duse64bitint), as Debian's m68k Perl has: 32-bit
# IVs overflow on file sizes, times in nanoseconds and every 64-bit
# checksum, and a great deal of CPAN assumes they do not.
#
# Nothing built lands in this directory: it all goes to the build tree
# under ~/m68k/src, and `make install` copies it onto the disk.

set -eu

cd "$(dirname "$0")"
HERE=$(pwd)
. ../cross.sh

VERSION=5.44.0
SHA256=505cf43912e9480495c344c70260452e32aa2a73c546a026b3f100053b23ce91
URL=https://www.cpan.org/src/5.0/perl-$VERSION.tar.xz
PC_VERSION=1.6.5
PC_SHA256=81130cd4b8c6d9eb2a1959f37d44391a46ad6a6794fc41fed5441f74e85e3dd0
PC_URL=https://github.com/arsv/perl-cross/releases/download/$PC_VERSION/perl-cross-$PC_VERSION.tar.gz
BUILD=$SRCDIR/build-perl-sage040
SRC=$BUILD/perl-$VERSION
OUT=$BUILD/sage040

fetch() {                       # fetch URL SHA256
    local tarball=$SRCDIR/$(basename "$1")
    [ -f "$tarball" ] || curl -L --fail -o "$tarball" "$1"
    echo "$2  $tarball" | sha256sum -c - > /dev/null
    echo "$tarball"
}

mkdir -p "$SRCDIR"
perl_tar=$(fetch "$URL" "$SHA256")
pc_tar=$(fetch "$PC_URL" "$PC_SHA256")

libc_fresh "$BUILD" || true

# perl-cross builds IN the source tree, so the tree is the build: Perl
# unpacked, perl-cross laid over it, then this port's patches. Perl's
# tarball makes its files read-only, which patch cannot write.
if [ ! -f "$SRC/.sage040-ready" ]; then
    rm -rf "$SRC"
    tar -C "$BUILD" -xf "$perl_tar"
    tar -C "$SRC" -xzf "$pc_tar" --strip-components=1
    chmod -R u+w "$SRC"
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -d "$SRC" -p1 -s < "$p"
    done
    touch "$SRC/.sage040-ready"
fi

# THE TOOLS, UNDER A TARGET TRIPLET. perl-cross finds a target's tools
# by prefix (m68k-linux-gcc, -ar, -nm ...), and its option parser
# splits a value at every '=', so a --with-cc that carried -mcpu=68040
# arrived in pieces. A wrapper holds the flags instead, and Config
# records a plain tool name that is easy to turn into the machine's own
# at install time (below).
#
# The wrapper does not say -D_GNU_SOURCE, unlike cross.sh's flags:
# every configure probe defines it itself and warned about the
# redefinition. It goes in ccflags instead, as Perl's own Linux hints
# put it.
TOOLS=$BUILD/tools
mkdir -p "$TOOLS"
cat > "$TOOLS/m68k-linux-gcc" <<EOF
#!/bin/sh
exec "$CROSS_CC" -mcpu=68040 $SPECS_CFLAGS -nostdinc -isystem $SAGE_LIBC/include -isystem $("$CROSS_CC" -print-file-name=include) "\$@"
EOF
chmod +x "$TOOLS/m68k-linux-gcc"
for t in ar nm ranlib readelf objdump strip; do
    ln -sf "$CROSS_BIN/m68k-elf-$t" "$TOOLS/m68k-linux-$t"
done
PATH=$TOOLS:$PATH
export PATH

# WHY EACH ANSWER IS GIVEN RATHER THAN FOUND:
#
#   osname, archname   perl-cross derives them from the triplet and got
#                      an empty osname -- no hints loaded, and an
#                      archlib of ".../m68k-".
#   lddlflags=-shared  libc/sage040.specs makes `gcc -shared` a shared
#                      library; ccdlflags=-Wl,-E exports perl's symbols
#                      to the XS objects.
#   d_gethostprotos    the probe tests for gethostbyaddr's prototype,
#   netdb_name_type    which this C library has not got, and concluded
#                      there were none -- so pp_sys.c declared its own
#                      gethostbyname(int) against the real one.
#   d_tzname           the probe calls `void foo() {}` with an argument;
#                      under gcc 15's C23 that is a function of NO
#                      arguments, the probe fails to compile, and a
#                      tzname the C library has reads as missing.
#   usrinc             where Errno finds errno.h (patches/01): without
#                      it, the host's glibc header.
#   XS-APItest,        exist only for Perl's own core test suite, and
#   XS-Typemap         are the two largest objects in it.
CONF=(--target=m68k-linux --prefix=/usr
      -Dosname=linux -Darchname=m68k-linux
      -Doptimize=-O2 -Duse64bitint
      -Accflags=-D_GNU_SOURCE
      -Dcccdlflags=-fPIC -Dccdlflags=-Wl,-E -Dlddlflags=-shared
      -Dd_gethostprotos=define "-Dnetdb_name_type=const char *"
      -Dd_tzname=define
      -Dusrinc="$SAGE_LIBC/include"
      --disable-mod=ext/XS-APItest,ext/XS-Typemap)
# Reconfigured when the versions, the patches or these answers change --
# not on any edit to this script, which reconfigured and rebuilt the
# whole of Perl for a change to the install steps below.
stamp="$VERSION $PC_VERSION $(cat "$HERE"/patches/*.patch | sha1sum) ${CONF[*]}"
if [ ! -f "$SRC/config.sh" ] || [ "$(cat "$SRC/.sage040-conf" 2>/dev/null)" != "$stamp" ]; then
    (cd "$SRC" && ./configure "${CONF[@]}" > "$BUILD/configure.log" 2>&1) \
        || { tail -30 "$BUILD/configure.log"; exit 1; }
    echo "$stamp" > "$SRC/.sage040-conf"
fi

make -C "$SRC" -j"$(nproc)" > "$BUILD/make.log" 2>&1 \
    || { grep -E ' error|Error [0-9]' "$BUILD/make.log" | head -20; exit 1; }

rm -rf "$BUILD/inst"
make -C "$SRC" install DESTDIR="$BUILD/inst" > "$BUILD/install.log" 2>&1 \
    || { tail -30 "$BUILD/install.log"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
cp -a "$BUILD/inst/usr/." "$OUT/"
# Perl installs its files read-only, which strip and sed cannot rewrite.
chmod -R u+w "$OUT"

# NO MANUAL PAGES: there is no man on the machine, and perldoc reads the
# POD in the library, which stays.
rm -rf "$OUT/share/man"

"$CROSS_BIN/m68k-elf-strip" "$OUT/bin/perl"
find "$OUT/lib/perl5" -name '*.so' -exec "$CROSS_BIN/m68k-elf-strip" --strip-unneeded {} +

# THE CONFIG THE MACHINE SEES.
#
# sh is bash's, because MakeMaker writes `SHELL = $Config{sh}` into
# every Makefile it makes and GNU make runs each recipe through it --
# and /bin/sh here is the system's own shell, which is not POSIX: it
# took a recipe's backslash-newline continuation as a newline, and ran
# Mkbootstrap as `perl "\n"`.
# Config.pm and Config_heavy.pl record how
# Perl was built, and MakeMaker builds an XS module ON the machine from
# exactly those values -- so they have to name the machine's tools, not
# this workstation's wrapper and header directory.
arch=$OUT/lib/perl5/$VERSION/m68k-linux
for f in "$arch/Config.pm" "$arch/Config_heavy.pl"; do
    [ -f "$f" ] || continue
    # '#' as the delimiter: with '|' the alternation's \| is a literal.
    sed -i -e "s#m68k-linux-\(gcc\|ar\|nm\|ranlib\|readelf\|objdump\|strip\)#\1#g" \
           -e "s#$SAGE_LIBC/include#/usr/include#g" \
           -e "s#^sh='/bin/sh'#sh='/bin/bash'#" "$f"
done
if grep -qF "$HOME" "$arch/Config.pm" "$arch/Config_heavy.pl" \
   || grep -q "m68k-linux-" "$arch/Config.pm" "$arch/Config_heavy.pl"; then
    echo "perl: Config still names a path on this workstation:" >&2
    grep -nE "$HOME|m68k-linux-" "$arch/Config.pm" "$arch/Config_heavy.pl" | head >&2
    exit 1
fi

echo "perl $VERSION -> $OUT"
du -sh "$OUT" | awk '{print "   total", $1}'
