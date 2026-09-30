#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# xfertest.sh - sendfile, splice, copy_file_range, mremap, memfd_create.
#
# libc/test/xfertest does the work; see the head of it. Then the host
# reads the two files it left on the disk and compares them, byte for
# byte, with the same pattern computed here: what the kernel moved,
# checked by code that did not move it.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-xfer.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/xfertest.log"
FIFO="$SCRATCH/xfer.fifo"
rm -f "$LOG"
BOOT_WAIT=${BOOT_WAIT:-4}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

pass=0
fail=0
check() {
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../libc/test xfertest || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /tmp
fsimg put -m 755 ../libc/test/xfertest /xfertest

rm -f "$FIFO"
mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$FIFO"

# A WHOLE LINE, as an extended regex: the command typed to produce a
# marker echoes it too, and a substring match returns on the echo --
# which sent the second program while the first was still running.
wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

sleep "$BOOT_WAIT"
printf 'echo XFER-START\r' >&3
printf '/xfertest; echo "XFER-EXIT $?"\r' >&3
wait_for "XFER-EXIT [0-9]+" 900
printf 'sync; echo "SYNC-DONE"\r' >&3
wait_for "SYNC-DONE" 300
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"

tr -d '\r' < "$LOG" > "$SCRATCH/xfer-clean.tmp"
echo "=== guest session ==="
sed -n '/XFER-START/,$p' "$SCRATCH/xfer-clean.tmp" | sed 's/^/  | /'

echo "=== checks: on the machine ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*) check "${line#  ok   }" 0 ;;
        "  FAIL "*) check "${line#  FAIL }" 1 ;;
    esac
done < <(sed -n '/^XFER-START/,/^XFER-EXIT/p' "$SCRATCH/xfer-clean.tmp")
grep -qE '^xfertest: [0-9]+ checks, 0 failed$' "$SCRATCH/xfer-clean.tmp"
check "ran to the end, nothing failed" $?

echo "=== checks: on the host ==="
pat() {                 # pat FIRST COUNT: the source file's bytes
    python3 -c 'import sys
f, n = int(sys.argv[1]), int(sys.argv[2])
sys.stdout.buffer.write(bytes(((i * 31 + (i >> 8) * 7 + 3) & 0xff) for i in range(f, f + n)))' "$1" "$2"
}
fsimg get /xf.dst "$SCRATCH/xf.dst" >/dev/null 2>&1
{ pat 0 20000; pat 100 50; } | cmp -s - "$SCRATCH/xf.dst"
check "/xf.dst on the disk is the 20000 bytes sendfile moved, then the 50" $?
fsimg get /xf.cfr "$SCRATCH/xf.cfr" >/dev/null 2>&1
pat 1000 8000 | cmp -s - "$SCRATCH/xf.cfr"
check "/xf.cfr is the 8000 bytes copy_file_range moved, from 1000" $?

# Two bus errors are the test's own: faults() touches the old address of
# a moved mapping and the tail of a shrunk one, in a child, and expects
# each to die. Any other is a real fault.
! grep -qE 'panic|address error' "$SCRATCH/xfer-clean.tmp" &&
    [ "$(grep -c 'bus error' "$SCRATCH/xfer-clean.tmp")" -eq 2 ]
check "no panic, and no fault but the two the test provokes" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
