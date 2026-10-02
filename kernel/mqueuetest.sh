#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# mqueuetest.sh - POSIX message queues (kernel/mqueue.c), by
# libc/test/mqtest on the machine: every line it prints is a check.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-mqueue.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/mqueuetest.log"; FIFO="$SCRATCH/mqueue.fifo"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "mqueuetest: fsimg $* failed" >&2; exit 1; }; }

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../libc/test mqtest >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /tmp; do fsimg mkdir $d; done
put_shells
fsimg put -m 755 ../libc/test/mqtest /mqtest

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
        sleep 0.2
    done
    return 1
}
wait_for 'kernel ready.*' 300
sleep 1
printf '/mqtest; echo "MQ-END $?"\r' >&3
wait_for 'MQ-END [0-9]+' 600
printf 'halt\r' >&3
sleep 2
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CL="$SCRATCH/mqueue-clean.tmp"
tr -d '\r' < "$LOG" > "$CL"
sed -n '/^mqtest: start/,/^mqtest: [0-9]* checks/p' "$CL" | sed 's/^/  | /'
echo "=== checks ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*) check "${line#  ok   }" 0 ;;
        "  FAIL "*) check "${line#  FAIL }" 1 ;;
    esac
done < <(sed -n '/^mqtest: start/,/^mqtest: [0-9]* checks/p' "$CL")
grep -qE '^mqtest: [0-9]+ checks, 0 failed$' "$CL"
check "mqtest ran to the end, nothing failed" $?
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
