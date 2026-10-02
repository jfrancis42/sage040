#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# pccheck.sh - every library port's .pc, checked by using it.
#
# For each package ports/pkg-config can see: it resolves (its Requires
# all exist), its include directory exists, and a program linked with
# exactly what it reports LINKS. The package's own archives go in
# whole (--whole-archive), because a program that references nothing
# pulls nothing out of an archive, and then a missing Requires -- a
# libcurl.pc that forgot zstd -- links perfectly. Whole, every object
# in it needs its undefined symbols found in what the .pc names.
#
# Host only; no machine is booted.

set -u
cd "$(dirname "$0")/.."
. ports/cross.sh
PC=ports/pkg-config
W=${SAGE_SCRATCH:-/tmp/scratch}/pccheck; rm -rf "$W"; mkdir -p "$W"
printf 'int main(void) { return 0; }\n' > "$W/main.c"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

# pkgconf lists itself as two virtual packages, with no files.
pkgs=$("$PC" --list-package-names | grep -vx "pkgconf\|pkg-config" | sort)
n=$(echo "$pkgs" | grep -c .)
[ "$n" -ge 30 ]; check "pkg-config sees the ports' packages ($n)" $?
# ... and none of the host's: zlib is on every host, so it is the probe.
zl=$("$PC" --variable=libdir zlib 2>/dev/null)
case "$zl" in */build-zlib-sage040/*) r=0 ;; *) r=1 ;; esac
check "zlib is the port's ($zl), not the host's" $r

for p in $pkgs; do
    "$PC" --exists "$p" 2>"$W/$p.err"; r=$?
    check "$p resolves$( [ $r -ne 0 ] && echo ": $(head -1 "$W/$p.err")")" $r
    [ $r -eq 0 ] || continue
    inc=$("$PC" --variable=includedir "$p")
    [ -d "$inc" ]; check "$p: include directory $inc" $?
    # The package's own archives, whole; then everything it reports.
    own=""
    for l in $("$PC" --libs-only-l --maximum-traverse-depth=1 "$p"); do
        a=$("$PC" --variable=libdir "$p")/lib${l#-l}.a
        [ -f "$a" ] && own="$own $a"
    done
    [ -n "$own" ] || [ "$p" = openssl ]; check "$p: its archive is there" $?
    # shellcheck disable=SC2046
    "$CROSS_CC" $SPECS_CFLAGS -mcpu=68040 $("$PC" --cflags "$p") \
        "$W/main.c" -o "$W/$p.out" \
        -Wl,--whole-archive $own -Wl,--no-whole-archive \
        $("$PC" --libs "$p") > "$W/$p.link" 2>&1
    r=$?
    check "$p: a program links with what it reports$( [ $r -ne 0 ] &&
        grep -m1 'undefined reference' "$W/$p.link" | sed 's/.*undefined/: undefined/')" $r
done

echo "  pccheck: $pass passed, $fail failed"
[ $fail -eq 0 ]
