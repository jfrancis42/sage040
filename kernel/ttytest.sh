#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# ttytest.sh - two independent terminals, not one mirrored console.
#
# The screen and the serial line used to be a single console fanned out
# to both, which clamped a full-screen program to the smaller of the two
# sizes and made a 80x30 screen behave as 80x24. They are separate
# terminals now, each with its own size and its own getty, and this
# proves the three things that make that true:
#
#   - each reports ITS OWN size: the serial line 80x24, the screen 80x30,
#     and neither clamps the other. The screen is asked through its node,
#     /dev/tty1, from the serial side -- no keyboard needed.
#   - they are ISOLATED: what is written to one does not appear on the
#     other. (That is the whole bug -- the old mirroring -- stated as a
#     test.)
#   - a getty runs on EACH, so there is somewhere to log in on both.
#
# Driven over the serial line with no display, like every suite; the
# screen is reached through its device node and the process table.
set -u
cd "$(dirname "$0")"
. ../machine.conf
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
S=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$S"
DISK="$S/hd-tty.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "ttytest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/tty.log"; F="$S/tty.fifo"; rm -f "$LOG" "$F" "$S/tty.state"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh stty >/dev/null || exit 1

rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin
fsimg put -m 755 ../system/sh   /bin/sh
fsimg put -m 755 ../system/stty /bin/stty
# echo and ps are shell built-ins; only sh and stty need installing.

mkfifo "$F"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
  -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
  -chardev stdio,id=con,signal=off -serial chardev:con \
  < "$F" > "$LOG" 2>&1 &
pid=$!
exec 3> "$F"; sleep 6
send(){ printf '%s\r' "$1" >&3; sleep "${2:-2}"; }

send 'echo ==SIZES'
send 'stty size'                    2   # this terminal: the serial line
send 'stty size < /dev/tty1'        2   # the screen, asked directly
send 'echo ==ISOLATION'
send 'echo LEAK-TO-SCREEN > /dev/tty1' 2   # goes to the screen, not here
send 'echo ==PS'
send 'ps'                           3
send 'echo ==DONE'
for i in $(seq 1 40); do grep -q '==DONE' "$LOG" && break; sleep 1; done
sleep 1
exec 3>&-; kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$F"
CLEAN="$S/tty-clean.txt"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' > "$CLEAN"

echo "=== guest session ==="
sed -n '/==SIZES/,$p' "$CLEAN" | sed 's/^/  | /'

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }
between(){ awk -v a="$1" -v b="$2" 'index($0,a){on=1;next} index($0,b){on=0} on' "$CLEAN"; }

echo "=== checks ==="

# The boot banner: two terminals, wired to their own devices.
grep -q "tty1: fbcon(out)" "$CLEAN"
check "the screen is its own terminal (tty1): keyboard and framebuffer" $?
grep -q "console: ttyS0(out) ttyS0(in)" "$CLEAN"
check "the serial line is its own terminal (console)" $?

# Sizes: each its own, no clamping.
sz=$(between '==SIZES' '==ISOLATION' | grep -m1 -E '^[0-9]+ [0-9]+$')
scr=$(between '==SIZES' '==ISOLATION' | grep -E '^[0-9]+ [0-9]+$' | sed -n 2p)
test "$sz" = "24 80";   check "the serial terminal is 80x24" $?
test "$scr" = "30 80";  check "the screen terminal (/dev/tty1) is 80x30 -- its real size" $?

# Isolation: what was written to the screen did not appear here.
# The leaked OUTPUT would be a bare line "LEAK-TO-SCREEN"; the command
# line that produced it necessarily contains the string too (the shell
# echoes what was typed), so match the output line exactly, not loosely.
if between '==ISOLATION' '==PS' | grep -qxF 'LEAK-TO-SCREEN'; then leak=1; else leak=0; fi
check "writing to the screen does NOT echo on the serial line -- no mirroring" $leak

# A getty on each: the boot task's serial shell, and a second task on the
# screen. ps shows both running.
[ "$(between '==PS' '==DONE' | grep -cE ' (sh|getty)$')" -ge 2 ] 2>/dev/null || \
  [ "$(between '==PS' '==DONE' | grep -ciE 'sh|getty')" -ge 2 ]
check "a getty runs on each terminal (two shell tasks)" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
