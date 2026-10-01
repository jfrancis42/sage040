#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fuzztest.sh - random system calls with hostile arguments, and the
# machine must come through them.
#
# libc/test/sysfuzz does the calling (see the head of it): rounds of a
# few thousand calls each, from a child that is a nobody in a chroot,
# with the disk-delay knob on so that tasks sleep inside the filesystem
# and the windows where races live are wide. What is checked:
#
#   - the machine does not panic, fault or double-fault, and is still
#     answering at the end (a shell command after the fuzzer);
#   - between rounds it is still itself, and after them its pages and
#     tasks are back (sysfuzz's own checks);
#   - the volume the children were scribbling on is clean by the HOST's
#     e2fsck after a halt -- a bad call that corrupts the filesystem
#     without crashing anything is the failure worth finding most.
#
# FUZZ_ROUNDS, FUZZ_CALLS, FUZZ_SEED and FUZZ_DELAY change the run; a
# seed that fails is printed, and FUZZ_ROUNDS=1 FUZZ_SEED=<it> repeats
# that round alone.

set -u
cd "$(dirname "$0")"
. ../machine.conf

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-fuzz.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/fuzztest.log"; FIFO="$SCRATCH/fuzz.fifo"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "fuzztest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
ROUNDS=${FUZZ_ROUNDS:-40}; CALLS=${FUZZ_CALLS:-3000}
SEED=${FUZZ_SEED:-1}; DELAY=${FUZZ_DELAY:-2}

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../libc/test sysfuzz >/dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /lib; fsimg mkdir /tmp; fsimg mkdir /fz
fsimg chown /fz 1000:1000
debugfs -w -R 'sif /fz mode 040777' "$DISK?offset=$OFF" >/dev/null 2>&1
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../libc/test/sysfuzz /sysfuzz
printf 'canary\n' > "$SCRATCH/canary.tmp"; fsimg put "$SCRATCH/canary.tmp" /fz-canary
# The chroot needs the loader and the library too: the children are
# forks of a dynamic program, but anything they map by name comes from
# inside /fz.
fsimg mkdir /fz/lib
fsimg put ../ldso/ld.so /fz/lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /fz/lib/libc.so

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {
    local i
    for i in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        tr -d '\r' < "$LOG" | grep -aqE 'panic|DOUBLE MMU FAULT|halted' && return 1
        sleep 0.2
    done
    return 1
}

wait_for 'kernel ready.*' 300
sleep 0.5
printf '/sysfuzz %s %s %s %s; echo "FUZZ-END $?"\r' "$ROUNDS" "$CALLS" "$SEED" "$DELAY" >&3
wait_for 'FUZZ-END [0-9]+' 30000
printf 'echo "STILL-HERE"\r' >&3
wait_for 'STILL-HERE' 300
printf 'sync; echo "SYNCED"\r' >&3
wait_for 'SYNCED' 600
printf 'halt\r' >&3
wait_for 'halting.*' 300
sleep 1
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CLEAN="$SCRATCH/fuzztest-clean.tmp"
tr -d '\r' < "$LOG" > "$CLEAN"
echo "=== guest session (the last 30 lines) ==="
sed -n '/^sysfuzz:/,$p' "$CLEAN" | tail -30 | sed 's/^/  | /'

echo "=== checks ==="
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow|unhandled exception' "$CLEAN"
check "no panic, no fault, no stack overflow" $?
n=$(grep -ac '^round [0-9]* seed' "$CLEAN")
[ "$n" = "$ROUNDS" ]
check "all $ROUNDS rounds of $CALLS calls ran (${n:-0})" $?
grep -aqx 'sysfuzz: ok' "$CLEAN" && grep -aqx 'FUZZ-END 0' "$CLEAN"
check "between and after rounds the machine was itself; pages and tasks came back" $?
grep -aqx 'STILL-HERE' "$CLEAN"
check "the console still answers afterwards" $?
p="$SCRATCH/fuzz.part"
dd if="$DISK" of="$p" bs=512 skip=2048 status=none
e2fsck -fn "$p" > "$SCRATCH/fuzz.fsck" 2>&1 &&
    ! grep -qE 'Fix\?|count wrong|differences|Unconnected|Unattached' "$SCRATCH/fuzz.fsck"
check "the volume is clean by the host's e2fsck" $?
if [ $fail -ne 0 ]; then
    last=$(grep -a '^round [0-9]* seed' "$CLEAN" | tail -1)
    echo "  the last round started: ${last:-none} -- FUZZ_ROUNDS=1 FUZZ_SEED=<seed> repeats it"
fi

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
