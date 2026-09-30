#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# devdirtest.sh - /dev as a directory, /dev/fd and /dev/stdin.
#
# libc/test/devdirtest does the work; see the head of it. Then the
# SHELL lists /dev, which is the first thing a person types.
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
DISK="$SCRATCH/hd-devdir.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/devdirtest.log"
FIFO="$SCRATCH/devdir.fifo"
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
make -s -C ../libc/test devdirtest || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /tmp
fsimg put -m 755 ../libc/test/devdirtest /devdirtest
fsimg mkdir /bin
fsimg put -m 755 ../ports/sbase/bin/ls /bin/ls
fsimg mkdir /lib
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so

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
printf 'echo DEVDIR-START\r' >&3
printf '/devdirtest; echo "DEVDIR-EXIT $?"\r' >&3
wait_for "DEVDIR-EXIT [0-9]+" 900
printf 'echo LS-START; /bin/ls /dev; echo LS-END\r' >&3
wait_for "LS-END" 300
printf 'sync; echo "SYNC-DONE"\r' >&3
wait_for "SYNC-DONE" 300
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"

tr -d '\r' < "$LOG" > "$SCRATCH/devdir-clean.tmp"
echo "=== guest session ==="
sed -n '/DEVDIR-START/,$p' "$SCRATCH/devdir-clean.tmp" | sed 's/^/  | /'

echo "=== checks: on the machine ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*) check "${line#  ok   }" 0 ;;
        "  FAIL "*) check "${line#  FAIL }" 1 ;;
    esac
done < <(sed -n '/^DEVDIR-START/,/^DEVDIR-EXIT/p' "$SCRATCH/devdir-clean.tmp")
grep -qE '^devdirtest: [0-9]+ checks, 0 failed$' "$SCRATCH/devdir-clean.tmp"
check "ran to the end, nothing failed" $?

sed -n '/^LS-START/,/^LS-END/p' "$SCRATCH/devdir-clean.tmp" | grep -qwE 'null' &&
    sed -n '/^LS-START/,/^LS-END/p' "$SCRATCH/devdir-clean.tmp" | grep -qwE 'pts'
check "/bin/ls /dev lists the devices and pts" $?

grep -qE 'panic|bus error|address error' "$SCRATCH/devdir-clean.tmp"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
