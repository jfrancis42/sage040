#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# panictest.sh - a panic is kept in the NVRAM and reported by the next
# boot.
#
# klog.c writes the end of what the kernel said into the last kilobyte
# of the M48T59's battery-backed RAM when it panics, and the next boot
# prints it and clears it. The panic made here is a real one -- a kernel
# stack overflow, from kstat KSTAT_STACK_PROBE -- so the record has to
# carry the overflow's report as well as the panic line. The machine is
# then reset through QEMU's monitor, which keeps the chip's RAM as a
# power cycle with a battery would.
#
# Checked: the record is printed by the next boot, with the panic and the
# report in it; it is printed ONCE (a third boot says nothing); and a
# setting /bin/nvram made before the panic is still there -- the kernel's
# kilobyte and /dev/nvram's 7152 bytes do not overlap.

set -u
cd "$(dirname "$0")"
. ../machine.conf

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-panic.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/panictest.log"; FIFO="$SCRATCH/panic.fifo"; MON="$SCRATCH/panic.mon"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "panictest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system nvram || exit 1
make -s -C ../libc/test kstackprobe >/dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /lib; fsimg mkdir /bin; fsimg mkdir /tmp
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../libc/test/kstackprobe /kstackprobe
fsimg put -m 755 ../system/nvram /bin/nvram

rm -f "$FIFO" "$MON"; mkfifo "$FIFO"
# NOT -no-reboot: the reset below has to start the machine again.
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -nic none \
    -monitor unix:"$MON",server,nowait \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
count() { tr -d '\r' < "$LOG" | grep -acxE -- "$1"; }
wait_n() {                      # wait_n REGEX N: until it has been seen N times
    local i
    for i in $(seq 1 900); do
        [ "$(count "$1")" -ge "$2" ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_n "$1-END [0-9]+" 1; }
reset() { echo system_reset | socat - unix:"$MON" >/dev/null 2>&1; }

wait_n 'kernel ready.*' 1
sleep 0.5
run NVSET '/bin/nvram greeting=kept-across'
printf '/kstackprobe 64\r' >&3
wait_n '\*\*\* halted\.' 1
check "the machine panics: a kernel stack overflow" $?
sleep 0.5
reset
wait_n 'kernel ready.*' 2
sleep 0.5
run NVGET '/bin/nvram greeting'
reset
wait_n 'kernel ready.*' 3
sleep 1
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO" "$MON"

CL="$SCRATCH/panictest-clean.tmp"
tr -d '\r' < "$LOG" > "$CL"
# The boots, each from its banner to the next.
awk '/^Sage040 boot ROM/ {n++} {print > ("'"$SCRATCH"'/panic-boot" n ".tmp")}' "$CL"
B2="$SCRATCH/panic-boot2.tmp"; B3="$SCRATCH/panic-boot3.tmp"
echo "=== the second boot's report ==="
sed -n '/the last boot ended in a panic/,/end of the panic record/p' "$B2" | sed 's/^/  | /'

echo "=== checks ==="
grep -q 'the last boot ended in a panic' "$B2"
check "the next boot reports the panic" $?
sed -n '/the last boot ended in a panic/,/end of the panic record/p' "$B2" |
    grep -q '^\*\*\* panic: kernel stack overflow$'
check "  the record holds the panic line" $?
sed -n '/the last boot ended in a panic/,/end of the panic record/p' "$B2" |
    grep -q 'kernel stack overflow: /kstackprobe (pid [0-9]*)' &&
sed -n '/the last boot ended in a panic/,/end of the panic record/p' "$B2" |
    grep -q 'return addresses'
check "  and what was said before it: the overflow's report, task and addresses" $?
! grep -q 'the last boot ended in a panic' "$B3"
check "the boot after that says nothing: the record was cleared" $?
grep -qx 'kept-across' "$B2"
check "a /bin/nvram setting made before the panic is still there" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
