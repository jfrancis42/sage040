#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# linetest.sh - lines drawn by the SM501's 2D engine, judged from a
# screendump.
#
# apps/lineprobe draws lines in every octant, each from both ends, and
# lines off every edge beside the same lines moved fully on screen;
# kernel/lineprobe.py reads the screendump and checks properties any
# correct line has (see its head). The engine's Line Draw is QEMU's --
# qemu-patch adds it -- so this is also the test of that.
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
DISK="$SCRATCH/hd-line.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/linetest.log"
MON="$SCRATCH/linetest.mon"
FIFO="$SCRATCH/line.fifo"
SHOT="$SCRATCH/lines.ppm"
rm -f "$LOG" "$SHOT" "$SCRATCH/linetest-qemu.log"
BOOT_WAIT=${BOOT_WAIT:-4}

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
make -s -C ../apps lineprobe || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg put -m 755 ../apps/lineprobe /lineprobe

rm -f "$FIFO" "$MON"
mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -d guest_errors,unimp -D "$SCRATCH/linetest-qemu.log" \
    -monitor "unix:$MON,server,nowait" \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$FIFO"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

sleep "$BOOT_WAIT"
printf '/lineprobe &\r' >&3
wait_for "LP-READY" 600
sleep 0.5
printf 'screendump %s\n' "$SHOT" | socat - "unix:$MON" >/dev/null
for _ in $(seq 1 50); do
    [ -s "$SHOT" ] && break
    sleep 0.2
done

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"

tr -d '\r' < "$LOG" > "$SCRATCH/line-clean.tmp"
echo "=== guest session ==="
grep '^lineprobe:' "$SCRATCH/line-clean.tmp" | sed 's/^/  | /'

echo "=== checks ==="
grep -qE '(^|\$ )lineprobe: all drawn$' "$SCRATCH/line-clean.tmp"
check "every line was accepted by the driver" $?
grep -qE '(^|\$ )lineprobe: far line refused$' "$SCRATCH/line-clean.tmp"
check "a line past the engine's coordinate range is refused" $?
[ -s "$SHOT" ]
check "the screendump was taken" $?

while IFS= read -r line; do
    case "$line" in
        "  ok   "*) check "${line#  ok   }" 0 ;;
        "  FAIL "*) check "${line#  FAIL }" 1 ;;
    esac
done < <(python3 lineprobe.py "$SHOT" 2>&1)

grep -qE 'panic|bus error|address error' "$SCRATCH/line-clean.tmp"
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
