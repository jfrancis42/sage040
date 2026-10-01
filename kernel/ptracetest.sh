#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# ptracetest.sh - ptrace(2): a tracer's view of a tracee.
#
# libc/test/ptracetest does the work; see the head of it. Syscall stops
# and changing and skipping a call; PEEK and POKE of data and of
# read-only code; a breakpoint and single steps; signal stops; FP
# registers; attach and detach; following a fork; PTRACE_KILL; and
# /proc's State and TracerPid.

set -u
# A write to the console FIFO after QEMU has died raises SIGPIPE, and
# that killed the suite before it printed a single check -- a machine
# that crashed read as silence. Ignored, the write fails and the checks
# say what went wrong.
trap '' PIPE

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-ptrace.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/ptracetest.log"
FIFO="$SCRATCH/ptrace.fifo"
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
make -s -C ../libc/test ptracetest || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /tmp
fsimg put -m 755 ../libc/test/ptracetest /ptracetest
fsimg mkdir /bin
fsimg put -m 755 ../ports/sbase/bin/true /bin/true
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
printf 'echo PTRACE-START\r' >&3
printf '/ptracetest; echo "PTRACE-EXIT $?"\r' >&3
wait_for "PTRACE-EXIT [0-9]+" 900
printf 'sync; echo "SYNC-DONE"\r' >&3
wait_for "SYNC-DONE" 300
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"

tr -d '\r' < "$LOG" > "$SCRATCH/ptrace-clean.tmp"
echo "=== guest session ==="
sed -n '/PTRACE-START/,$p' "$SCRATCH/ptrace-clean.tmp" | sed 's/^/  | /'

echo "=== checks: on the machine ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*) check "${line#  ok   }" 0 ;;
        "  FAIL "*) check "${line#  FAIL }" 1 ;;
    esac
done < <(sed -n '/^PTRACE-START/,/^PTRACE-EXIT/p' "$SCRATCH/ptrace-clean.tmp")
grep -qE '^ptracetest: [0-9]+ checks, 0 failed$' "$SCRATCH/ptrace-clean.tmp"
check "ran to the end, nothing failed" $?


grep -qE 'panic|bus error|address error|DOUBLE MMU FAULT' "$SCRATCH/ptrace-clean.tmp"
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
