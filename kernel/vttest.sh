#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# vttest.sh - the framebuffer console as a VT102.
#
# Two halves, because a terminal emulator can be wrong in two ways.
#
# `vtcheck` writes escape sequences to /dev/fbcon and reads what they did
# back from /dev/vcsa -- every cell's character and attribute, and the
# cursor. That catches the emulation being wrong.
#
# It cannot catch the PIXELS being wrong, which is the other way: scrolls,
# inserts and deletes are done by the blitter, and a blit that moves the
# wrong rectangle leaves the character buffer perfectly right and the
# screen wrong. So `vtcheck blit` does every one of them, the screen is
# captured, the console is told to redraw itself from its character
# buffer -- right by construction -- and captured again. The two must be
# identical to the byte.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"

. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-vt.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/vttest.log"
MON="$SCRATCH/vt-mon.sock"
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
make -s -C ../apps || exit 1
make -s -C ../system || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" ../apps/vtcheck ::/VTCHECK

rm -f "$SCRATCH/in.fifo" "$MON" "$SCRATCH"/vt-shot*.ppm
mkfifo "$SCRATCH/in.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -monitor "unix:$MON,server,nowait" \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 150); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

shot() {
    printf 'screendump %s\n' "$1" | socat - "unix:$MON" >/dev/null
    # screendump writes the file asynchronously enough to be worth a wait
    for _ in $(seq 1 25); do
        [ -s "$1" ] && break
        sleep 0.2
    done
}

printf 'vtcheck\r' >&3
wait_for "vtcheck: done"
sleep 0.5

printf 'vtcheck blit\r' >&3
wait_for "vtcheck: ready for the first screenshot"
sleep 0.5
shot "$SCRATCH/vt-shot1.ppm"
printf 'x' >&3
wait_for "vtcheck: redrawn, ready for the second"
sleep 0.5
shot "$SCRATCH/vt-shot2.ppm"
printf 'x' >&3
wait_for "vtcheck: blit done"

printf 'echo VT-FINISHED\r' >&3
wait_for "VT-FINISHED"
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo" "$MON"

tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/clean.tmp"

echo "=== checks: the emulation, through /dev/vcsa ==="

# Each line vtcheck prints is one check, named by its own text.
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(grep -E '^  (ok|FAIL) ' "$SCRATCH/clean.tmp")

test "$(grep -cE '^  ok   ' "$SCRATCH/clean.tmp")" -ge 50
check "vtcheck ran all of its checks" $?

grep -qF "vtcheck: done" "$SCRATCH/clean.tmp"
check "and finished" $?

echo "=== checks: the pixels, blitter against redraw ==="

test -s "$SCRATCH/vt-shot1.ppm" && test -s "$SCRATCH/vt-shot2.ppm"
check "both screenshots were taken" $?

# Not a black screen: something was drawn, so identical means something.
mean=$(magick "$SCRATCH/vt-shot1.ppm" -format '%[fx:mean]' info: 2>/dev/null)
echo "  mean brightness of the first: ${mean:-?}"
awk -v m="${mean:-0}" 'BEGIN { exit !(m > 0.02) }'
check "the pattern reached the screen" $?

cmp -s "$SCRATCH/vt-shot1.ppm" "$SCRATCH/vt-shot2.ppm"
check "scrolls, inserts and deletes by blitter match a full redraw" $?
if ! cmp -s "$SCRATCH/vt-shot1.ppm" "$SCRATCH/vt-shot2.ppm"; then
    magick compare -metric AE "$SCRATCH/vt-shot1.ppm" "$SCRATCH/vt-shot2.ppm" \
        "$SCRATCH/vt-diff.png" 2>&1 | sed 's/^/  differing pixels: /'
    echo "  difference image: $SCRATCH/vt-diff.png"
fi

grep -qx "VT-FINISHED" "$SCRATCH/clean.tmp"
check "the shell is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
