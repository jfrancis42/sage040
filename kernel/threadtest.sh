#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# threadtest.sh - threads on this kernel: clone(2), futexes, and the
# pthread layer over them.
#
# libc/test/threadtest is an ordinary POSIX program. Every line it
# prints is a check, and it carries its own negative control: the same
# counter summed WITHOUT a mutex, which has to come out short, or the
# locked one proved nothing.
#
# What this script adds, from outside the program:
#
#   - `ps` while threads are running, which is how a thread being a TASK
#     is visible: several rows, one process id.
#   - that the program's exit takes its threads with it -- a running
#     thread left behind would show in a later `ps`, and the task table
#     would never empty.
#   - that the machine is still there afterwards, with no panic.

set -u

cd "$(dirname "$0")"

. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-thread.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/threadtest.log"
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
if ! make -s -C ../libc/test threadtest; then
    echo "threadtest.sh: could not build against picolibc -- run make libc" >&2
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
mcopy -o -i "$MIMG" ../libc/test/threadtest ::/THREADTS

rm -f "$SCRATCH/thread.fifo"
mkfifo "$SCRATCH/thread.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/thread.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/thread.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

# The task table before anything runs, to compare with afterwards.
printf 'ps\r' >&3
sleep 0.5
printf 'echo PS-BEFORE-DONE\r' >&3
wait_for "PS-BEFORE-DONE"

printf '/THREADTS\r' >&3
# While it is running its last section, a busy thread of its own is
# alive: ask the kernel what tasks exist. The program is on its own
# console, so this arrives as input to it and is ignored -- which is
# why ps comes after it finishes, below.
wait_for "RESULT: " 1800
sleep 0.5

printf 'ps\r' >&3
sleep 0.5
printf 'echo PS-AFTER-DONE\r' >&3
wait_for "PS-AFTER-DONE"
printf 'echo STILL-HERE\r' >&3
wait_for "STILL-HERE"
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/thread.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/thread-clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/thread-clean.tmp"

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
done < <(grep -E "^  \[( OK |FAIL)\] " "$SCRATCH/thread-clean.tmp")

grep -qx "RESULT: PASS" "$SCRATCH/thread-clean.tmp"
check "threadtest ran to the end" $?

echo "=== checks: from outside the program ==="

# Every thread the program left behind would be a task. The table after
# is the table before: nothing of it survives its exit, which is what
# exit_group means.
# Each `ps` prints a header and then one row per task; count the rows of
# the first block and of the last, which is after the program has gone.
count_tasks() {                 # count_tasks NTH
    awk -v want="$1" '
        /^ *PID +PPID +STATE/ { block++; next }
        block == want && $1 ~ /^[0-9]+$/ { n++ }
        END { print n + 0 }
    ' "$SCRATCH/thread-clean.tmp"
}
before=$(count_tasks 1)
after=$(count_tasks 2)
echo "         tasks before: $before, after: $after"
test "$before" -gt 0 && test "$after" -eq "$before"
check "no thread outlived the program that made it ($before tasks, then $after)" $?

! grep -qE "panic|exception at|DOUBLE MMU" "$SCRATCH/thread-clean.tmp"
check "no panic, no kernel exception" $?

grep -qx "STILL-HERE" "$SCRATCH/thread-clean.tmp"
check "the shell is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
