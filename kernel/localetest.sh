#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# localetest.sh - locale(1) says what the C library will actually do.
#
# Every locale `locale -a` lists must be one setlocale accepts, and one
# it does not (zh_TW.big5: there is no Big5 here) must not be listed.
# `locale charmap` under a LANG is checked against the charset that LANG
# names, and `locale` alone against POSIX's rules for which variable
# decides a category.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-locale.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/localetest.log"; FIFO="$SCRATCH/locale.fifo"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "localetest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../utils >/dev/null || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /tmp
put_shells bash
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../utils/locale /bin/locale

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {
    local i
    for i in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
# Through bash, for its VAR=value prefixes.
run() { printf "/bin/sh -c '%s'; echo \"%s\"-END \$?\r" "$2" "$1" >&3; wait_for "$1-END [0-9]+" 600; }

wait_for 'kernel ready.*' 600
sleep 0.5
run ALL 'locale -a'
run MAPS 'locale -m'
run CM1 'LANG=de_DE.ISO-8859-1 locale charmap'
run CM2 'LANG=en_US.UTF-8 locale charmap'
run CM3 'LC_ALL=C locale charmap'
run CM4 'LANG=zh_TW.big5 locale charmap'
run ENV 'LANG=en_US.UTF-8 LC_TIME=C.ISO-8859-15 locale'
run DP 'LANG=C locale -k decimal_point'
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CL="$SCRATCH/locale-clean.tmp"
tr -d '\r' < "$LOG" > "$CL"
block() { sed -n "/echo \"$1\"-END/,/^$1-END/p" "$CL" | sed '1d;$d'; }
echo "=== guest session ==="
sed -n '/locale -a/,$p' "$CL" | head -80 | sed 's/^/  | /'

echo "=== checks ==="
block ALL | grep -qx 'C.UTF-8' && block ALL | grep -qx 'C.ISO-8859-1' &&
    block ALL | grep -qx POSIX && block ALL | grep -qx C
check "locale -a lists C, POSIX, C.UTF-8 and C.ISO-8859-1" $?
! block ALL | grep -qi 'big5'
check "  and not Big5, which the C library has not got" $?
# The count FIRST: a $(...) in the label would reset $? before check
# reads it, and the check would pass whatever happened.
nall=$(block ALL | wc -l); [ "$nall" -ge 40 ]; r=$?
check "  and the forty-odd charsets it has ($nall locales)" $r
cm1=$(block CM1 | head -1); [ "$cm1" = ISO-8859-1 ]; r=$?
check "LANG=de_DE.ISO-8859-1: charmap ISO-8859-1 (got $cm1)" $r
[ "$(block CM2)" = UTF-8 ]
check "LANG=en_US.UTF-8: charmap UTF-8" $?
cm3=$(block CM3 | head -1)
echo "$cm3" | grep -qxE 'ANSI_X3\.4-1968|ASCII|US-ASCII'
check "LC_ALL=C: charmap ASCII, under whichever name ($cm3)" $?
block CM4 | grep -q 'Cannot set LC_ALL'
check "LANG=zh_TW.big5: refused, and said so" $?
block ENV | grep -qx 'LANG=en_US.UTF-8' && block ENV | grep -qx 'LC_TIME=C.ISO-8859-15' &&
    block ENV | grep -qx 'LC_CTYPE="en_US.UTF-8"'
check "locale: LC_TIME as set, the rest from LANG and quoted, as POSIX has it" $?
[ "$(block DP)" = 'decimal_point="."' ]
check "locale -k decimal_point: decimal_point=\".\"" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
