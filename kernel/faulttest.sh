#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# faulttest.sh - a program's faults, as signals it can catch and survive.
#
# A fault used to end the program whatever it had asked for. Now, if it
# has a handler, a fault becomes that signal -- SIGSEGV, SIGBUS, SIGILL,
# SIGFPE, SIGTRAP -- delivered on the spot with the fault's address and
# si_code, and the handler can repair the cause and return, so that the
# instruction runs again, or move the program on. apps/faulttest does
# seven of those and checks each one did what it should; the checks
# below read its verdicts, and that it got to the end.
#
# Without a handler the program still ends, with the registers printed,
# exactly as before: kernel/vmtest.sh covers that, unchanged.
#
# Not testable here: the 68040's pending write-backs (kernel/wb040.c),
# which QEMU never leaves; and SIGBUS for an odd program counter, which
# QEMU runs rather than faults on.
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
DISK="$SCRATCH/hd-fault.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/faulttest.log"
FIFO="$SCRATCH/fault.fifo"
BOOT_WAIT=${BOOT_WAIT:-4}
# Gone before QEMU starts, so no check can read a previous run's.
rm -f "$LOG"

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
make -s -C ../apps faulttest >/dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK" "$FIFO"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM >/dev/null
fsimg mkdir /bin >/dev/null
fsimg put -m 755 ../apps/faulttest /bin/faulttest >/dev/null

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qp=$!
exec 3> "$FIFO"

wait_for() {
    for _ in $(seq 1 300); do
        grep -qF "$1" "$LOG" 2>/dev/null && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

sleep "$BOOT_WAIT"
# Markers typed with an empty pair of quotes, so only OUTPUT matches.
printf "echo BOO''TED\r" >&3; wait_for BOOTED
printf 'faulttest; echo "status $?"\r' >&3
printf "echo DO''NE\r" >&3; wait_for DONE
exec 3>&-
kill "$qp" 2>/dev/null
wait "$qp" 2>/dev/null
rm -f "$FIFO"

tr -d '\r' < "$LOG" > "$SCRATCH/fault.clean"
echo "=== what it said ==="
sed -n '/^faulttest\|^  ok\|^  FAIL\|^status\|pc=/p' "$SCRATCH/fault.clean" | sed 's/^/  | /'

echo "=== checks ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(grep -E '^  (ok|FAIL) ' "$SCRATCH/fault.clean")

n=$(grep -cE '^  (ok|FAIL) ' "$SCRATCH/fault.clean")
[ "$n" -eq 12 ]
check "all twelve of its checks ran ($n)" $?
grep -q '^faulttest: all right' "$SCRATCH/fault.clean" &&
    grep -q '^status 0' "$SCRATCH/fault.clean"
check "and it said so, and exited 0" $?
! grep -qE 'panic|exception at' "$SCRATCH/fault.clean"
check "no panic" $?

echo
echo "--- $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ "$fail" -eq 0 ]
