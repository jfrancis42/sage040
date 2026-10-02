#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# xxhtest.sh - xxHash on a big-endian machine, and rsync's use of it.
#
# A hash ported to a big-endian CPU is wrong in a way nothing notices
# until two machines disagree. So: xxhsum here and xxhsum on the host,
# both from the same source, over the same files -- empty, one byte,
# every length either side of the 16-, 32-, 64- and 240-byte paths the
# algorithms branch on, and a megabyte -- for XXH32, XXH64, XXH128 and
# XXH3; every digest must agree. Then rsync: it lists xxh128, xxh3 and
# xxh64 among its checksums, and a copy made with each is byte for byte
# the original.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-xxh.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/xxhtest.log"; FIFO="$SCRATCH/xxh.fifo"; WORK="$SCRATCH/xxh.tmp"
rm -rf "$LOG" "$WORK"; mkdir -p "$WORK/files"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "xxhtest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
XXSRC=$SRCDIR/xxHash-0.8.3

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
../ports/xxhash/build.sh >/dev/null || exit 1
../ports/rsync/build.sh >/dev/null || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
cc -O2 -I"$XXSRC" "$XXSRC"/cli/*.c "$XXSRC"/xxhash.c -o "$WORK/xxhsum-host" ||
    { echo "xxhtest: no host xxhsum" >&2; exit 1; }

# The files: every length around the algorithms' branch points, and a
# megabyte of noise.
: > "$WORK/files/empty"
for n in 1 3 4 7 8 15 16 17 31 32 33 63 64 65 128 129 239 240 241 255 256 1000 4096; do
    head -c "$n" /dev/urandom > "$WORK/files/len$n"
done
head -c 1048576 /dev/urandom > "$WORK/files/meg"
FILES=$(cd "$WORK/files" && ls | sort | tr '\n' ' ')
(cd "$WORK/files" && for h in 0 1 2 3; do "$WORK/xxhsum-host" -H$h $FILES; done) > "$WORK/host.sums"

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=24 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /tmp /X /X/files; do fsimg mkdir $d; done
put_shells bash
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
for p in cmp ls cat; do fsimg put -m 755 ../ports/sbase/bin/$p /bin/$p; done
fsimg put -m 755 "$SRCDIR/build-xxhash-sage040/sage040/bin/xxhsum" /bin/xxhsum
fsimg put -m 755 "$SRCDIR/build-rsync-sage040/sage040/bin/rsync" /bin/rsync
fsimg put "$WORK"/files/* /X/files/

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {
    local i
    for i in $(seq 1 "${2:-900}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_for "$1-END [0-9]+" "${3:-900}"; }
wait_for 'kernel ready.*' 300
sleep 0.5
# Through bash: the console's shell has no for loop.
run SUMS "/bin/bash -c 'cd /X/files; for h in 0 1 2 3; do /bin/xxhsum -H\$h $FILES; done > /X/sums'" 3000
run VER '/bin/rsync -V'
for c in xxh128 xxh3 xxh64; do
    run "R$c" "/bin/rsync -a --checksum-choice=$c /X/files/ /tmp/$c/ && /bin/cmp /X/files/meg /tmp/$c/meg && /bin/cmp /X/files/len241 /tmp/$c/len241" 3000
done
run SYNC 'sync'
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

fsimg get /X/sums "$WORK/machine.sums" >/dev/null 2>&1
CL="$WORK/clean"; tr -d '\r' < "$LOG" > "$CL"
status() { sed -n "s/^$1-END //p" "$CL" | tail -1; }

echo "=== checks ==="
nh=$(wc -l < "$WORK/host.sums")
[ -s "$WORK/machine.sums" ] && cmp -s "$WORK/host.sums" "$WORK/machine.sums"; r=$?
check "xxhsum: all $nh digests (XXH32, XXH64, XXH128, XXH3 of $(echo $FILES | wc -w) files) match the host's" $r
[ $r -ne 0 ] && diff "$WORK/host.sums" "$WORK/machine.sums" | head -8 | sed 's/^/        /'
sed -n '/rsync -V/,/VER-END/p' "$CL" | grep -qE 'xxh128 xxh3 xxh64'
check "rsync -V offers xxh128, xxh3 and xxh64" $?
for c in xxh128 xxh3 xxh64; do
    [ "$(status R$c)" = 0 ]
    check "  a copy with --checksum-choice=$c is byte for byte the original" $?
done
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
