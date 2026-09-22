#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# libctest.sh - a real C library, on this kernel.
#
# libc/test/libctest is built against picolibc (libc/build.sh) and
# written the way a program from elsewhere is: <stdio.h>, fork, exec,
# signals, directories. Every line it prints is a check. Then it is run
# again with its output redirected to a file, and the file is read with
# the HOST's tools -- which is how a buffered stdout that never got
# flushed at exit would show, and nothing inside the machine could see.
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
DISK="$SCRATCH/hd-libc.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/libctest.log"
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
if ! make -s -C ../libc/test; then
    echo "libctest.sh: could not build against picolibc -- run libc/build.sh" >&2
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
mcopy -o -i "$MIMG" ../libc/test/libctest ::/LIBCTEST
mmd -i "$MIMG" ::/BIN

rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 300); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

printf 'libctest\r' >&3
wait_for "libctest: done"
sleep 0.5
printf 'libctest > /LCOUT.TXT\r' >&3
sleep 8
printf 'echo LIBC-FINISHED\r' >&3
wait_for "LIBC-FINISHED"
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"
mtype -i "$MIMG" ::/LCOUT.TXT 2>/dev/null | tr -d '\r' > "$SCRATCH/lcout.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/clean.tmp"

echo "=== checks: libctest ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(grep -E '^  (ok  |FAIL) ' "$SCRATCH/clean.tmp")

test "$(grep -cE '^  ok   ' "$SCRATCH/clean.tmp")" -ge 60
check "libctest ran all of its checks" $?

grep -qx "libctest: 0 failed" "$SCRATCH/clean.tmp"
check "a line printed with no newline was flushed before the next" $?

grep -qx "libctest: atexit handler ran" "$SCRATCH/clean.tmp"
check "atexit handlers run" $?

echo "=== checks: stdout to a file, read on the host ==="

grep -qx "libctest: picolibc on Sage040" "$SCRATCH/lcout.tmp"
check "the redirected output reached the file" $?

grep -qx "libctest: atexit handler ran" "$SCRATCH/lcout.tmp"
check "  all of it, including what was printed at exit" $?

test "$(grep -cE '^  FAIL ' "$SCRATCH/lcout.tmp")" -eq 0 &&
    test "$(grep -cE '^  ok   ' "$SCRATCH/lcout.tmp")" -ge 60
check "  and every check passed there too" $?

test "$(grep -c 'libctest: done' "$SCRATCH/lcout.tmp")" -eq 1
check "  exactly once: fork did not duplicate unflushed output" $?

grep -qx "LIBC-FINISHED" "$SCRATCH/clean.tmp"
check "the shell is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
