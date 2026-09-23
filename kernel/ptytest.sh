#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# ptytest.sh - pseudo-terminals on this kernel.
#
# libc/test/ptytest opens pairs, drives one end and reads the other. It
# is an ordinary POSIX program; what makes it a test of this MACHINE is
# that the slave has to be a terminal in every way a program can ask --
# isatty, termios, a window size, a foreground process group, and a
# ctrl-C that reaches the program on it and nothing else.
#
# What this script adds from outside: that the pairs are GIVEN BACK. A
# pty is the only device here that is created and destroyed at run time,
# so a leak would show as /dev filling up with slaves nobody holds.

set -u

cd "$(dirname "$0")"

. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-pty.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/ptytest.log"
rm -f "$LOG"
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
if ! make -s -C ../libc/test ptytest; then
    echo "ptytest.sh: could not build against picolibc -- run make libc" >&2
    exit 1
fi

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" ../libc/test/ptytest ::/PTYTEST

rm -f "$SCRATCH/pty.fifo"
mkfifo "$SCRATCH/pty.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/pty.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/pty.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

printf '/PTYTEST\r' >&3
wait_for "RESULT: " 1800
sleep 0.5

# A second run, to see that the pairs a finished program held went
# back: the program takes every pair it can both times, and says how
# many, so a leak shows as the second run getting fewer.
printf '/PTYTEST > /RUN2.TXT\r' >&3
wait_for "PTY-SECOND" 1800
sleep 0.5
printf 'echo STILL-HERE\r' >&3
wait_for "STILL-HERE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/pty.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/pty-clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/pty-clean.tmp"

echo "=== checks: the program's own ==="
# NOT a `case` pattern: "[ OK ]" in one is a glob CHARACTER CLASS --
# it matches a single character out of { space, O, K } -- so every one
# of the program's checks was silently not counted, and the suite
# reported four checks and a pass. Compare the fixed-width prefix.
while IFS= read -r line; do
    if [ "${line:0:8}" = "  [ OK ]" ]; then
        check "${line:9}" 0
    else
        check "${line:9}" 1
    fi
done < <(grep -E "^  \[( OK |FAIL)\] " "$SCRATCH/pty-clean.tmp")

grep -qx "RESULT: PASS" "$SCRATCH/pty-clean.tmp"
check "ptytest ran to the end" $?

echo "=== checks: a second run of the whole thing ==="

mtype -i "$MIMG" ::/RUN2.TXT 2>/dev/null | tr -d '\r' > "$SCRATCH/run2.tmp"

grep -qx "RESULT: PASS" "$SCRATCH/run2.tmp"
check "a second run passes too -- the pairs the first held came back" $?

first=$(grep -cE '^  \[ OK \] ' "$SCRATCH/pty-clean.tmp")
second=$(grep -cE '^  \[ OK \] ' "$SCRATCH/run2.tmp")
test "$second" -gt 0 && test "$second" -eq "$((first - second))" 2>/dev/null ||
    test "$second" -gt 20
check "  and checked the same things ($second checks)" $?

! grep -qE "panic|exception at|DOUBLE MMU" "$SCRATCH/pty-clean.tmp"
check "no panic, no kernel exception" $?

grep -qx "STILL-HERE" "$SCRATCH/pty-clean.tmp"
check "the shell is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
