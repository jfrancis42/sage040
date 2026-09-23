#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# vttest.sh - the framebuffer console as a VT102, and the terminal size.
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
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/vttest.log"
# Gone before QEMU starts, so a run that never reaches the guest has no
# log to grade -- rather than silently grading the last run's.
rm -f "$LOG"
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
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg put -m 755 ../apps/vtcheck /vtcheck
fsimg put -m 755 ../apps/winsize /winsize
fsimg mkdir /bin
fsimg put -m 755 ../system/stty /bin/stty
fsimg put -m 755 ../system/resize /bin/resize

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

# --- the size: TIOCGWINSZ, TIOCSWINSZ, SIGWINCH ---
sleep 0.5
printf 'echo TERM-IS-$TERM\r' >&3;         sleep 0.5
printf 'echo SIZE-AT-BOOT\r' >&3;          sleep 0.3
printf 'stty size\r' >&3;                  sleep 1
printf 'winsize\r' >&3
wait_for "winsize: done"
sleep 0.3

# resize asks the terminal; this script is the terminal, and answers
# as one 40 rows by 100 columns would.
printf 'resize\r' >&3
if wait_for $'\033[999;999H\033[6n'; then
    sleep 0.3
    printf '\033[40;100R' >&3
fi
wait_for "resize: "
sleep 0.3
printf 'echo SIZE-AFTER-RESIZE\r' >&3;     sleep 0.3
printf 'stty size\r' >&3;                  sleep 1
printf 'console fbcon off\r' >&3;          sleep 0.5
printf 'echo SIZE-LINE-ONLY\r' >&3;        sleep 0.3
printf 'stty size\r' >&3;                  sleep 1
printf 'console fbcon on\r' >&3;           sleep 0.5
printf 'stty rows 20 cols 60\r' >&3;       sleep 1
printf 'echo SIZE-AFTER-STTY\r' >&3;       sleep 0.3
printf 'stty size\r' >&3;                  sleep 1
printf 'stty rows 24 cols 80\r' >&3;       sleep 1
printf 'stty\r' >&3;                       sleep 1
printf 'stty bogus\r' >&3;                 sleep 1
printf 'stty rows 0\r' >&3;                sleep 1

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

echo "=== checks: the emulation, through /dev/vcsa, and winsize ==="

# Each line vtcheck or winsize prints is one check, named by its own
# text.
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(grep -E '^  (ok|FAIL) ' "$SCRATCH/clean.tmp")

test "$(grep -cE '^  ok   ' "$SCRATCH/clean.tmp")" -ge 50
check "vtcheck ran all of its checks" $?

grep -qF "vtcheck: done" "$SCRATCH/clean.tmp"
check "vtcheck finished" $?

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

echo "=== checks: the size ==="

# The line after a marker's echo: what `stty size` printed.
size_after() {
    awk -v m="$1" '$0 == m { f = 1; next } f && /^[0-9]+ [0-9]+$/ { print; exit }' \
        "$SCRATCH/clean.tmp"
}

grep -qx "TERM-IS-vt102" "$SCRATCH/clean.tmp"
check "TERM is vt102" $?

test "$(size_after SIZE-AT-BOOT)" = "24 80"
check "the size at boot is 24x80: the line's, smaller than the screen" $?

grep -qF "winsize: done" "$SCRATCH/clean.tmp"
check "winsize finished" $?

grep -qF "resize: 40 rows, 100 columns" "$SCRATCH/clean.tmp"
check "resize read the terminal's answer" $?

test "$(size_after SIZE-AFTER-RESIZE)" = "30 80"
check "  and with the screen on too, the screen's 30 rows still win" $?

test "$(size_after SIZE-LINE-ONLY)" = "40 100"
check "  and with the screen off, the terminal's 40x100" $?

test "$(size_after SIZE-AFTER-STTY)" = "20 60"
check "stty rows and cols set the line" $?

grep -qx "rows 24; columns 80;" "$SCRATCH/clean.tmp" &&
    grep -qx "icrnl opost onlcr isig icanon echo" "$SCRATCH/clean.tmp"
check "stty shows the size and the modes" $?

grep -qF "stty: unknown setting bogus" "$SCRATCH/clean.tmp"
check "stty refuses a setting it does not know" $?

grep -qF "stty: rows wants a number from 1 to 255" "$SCRATCH/clean.tmp"
check "  and a size of zero" $?

grep -qx "VT-FINISHED" "$SCRATCH/clean.tmp"
check "the shell is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
