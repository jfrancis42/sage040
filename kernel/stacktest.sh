#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# stacktest.sh - a kernel stack that runs out is REPORTED.
#
# Every kernel function compares the stack pointer with a limit kept in
# a5 and traps below it (task.c, KSTACK_RED). Before that, running off
# the end of a kernel stack reached the guard page beneath it, and a
# fault taken while pushing a fault frame is a double fault: QEMU
# printed "DOUBLE MMU FAULT" and exited, and the kernel said nothing.
#
# libc/test/kstackprobe has the kernel recurse until it has used a given
# number of kilobytes (kstat KSTAT_STACK_PROBE). A little must come back,
# and the high-water mark must see it; too much must stop the machine
# with a report that names the task, says how deep it got, and gives
# return addresses that the HOST's addr2line resolves to the function
# that recursed -- and never with a double fault.

set -u
cd "$(dirname "$0")"
. ../machine.conf

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-stack.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/stacktest.log"; FIFO="$SCRATCH/stack.fifo"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "stacktest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
ADDR2LINE=$M68K_PREFIX/bin/m68k-elf-addr2line
[ -x "$ADDR2LINE" ] || ADDR2LINE=m68k-elf-addr2line

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../libc/test kstackprobe >/dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /lib; fsimg mkdir /tmp
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../libc/test/kstackprobe /kstackprobe

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {                    # a WHOLE line, as a regex
    local i
    for i in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_for "$1-END [0-9]+" 600; }

wait_for 'kernel ready.*' 600
sleep 0.5
run SMALL '/kstackprobe 4'
run MID '/kstackprobe 16'
printf '/kstackprobe 64\r' >&3
wait_for '\*\*\* halted\.' 600
sleep 0.5
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CLEAN="$SCRATCH/stacktest-clean.tmp"
tr -d '\r' < "$LOG" > "$CLEAN"
echo "=== guest session ==="
sed -n '/kstackprobe 4/,$p' "$CLEAN" | sed 's/^/  | /'

echo "=== checks ==="
grep -qx 'SMALL-END 0' "$CLEAN"
check "4 KB of kernel stack: the call comes back" $?
grep -qx 'MID-END 0' "$CLEAN"
check "16 KB: the call comes back" $?
deep=$(sed -n 's/^back: deepest \([0-9]*\) of \([0-9]*\) bytes, by .*kstackprobe$/\1/p' "$CLEAN" | tail -1)
[ -n "$deep" ] && [ "$deep" -ge 16384 ]
check "  and the high-water mark saw it: ${deep:-nothing} bytes, by kstackprobe" $?

grep -q '^\*\*\* kernel stack overflow: .*kstackprobe (pid [0-9]*)$' "$CLEAN"
check "64 KB: REPORTED, naming the task" $?
grep -q '^\*\*\* panic: kernel stack overflow$' "$CLEAN"
check "  and the machine stops with a panic that says why" $?
! grep -q 'DOUBLE MMU FAULT' "$CLEAN"
check "  not a double fault (QEMU dying silently)" $?
line=$(grep -m1 'in the function at' "$CLEAN")
used=$(echo "$line" | sed -n 's/.* \([0-9]*\) of \([0-9]*\) bytes used$/\1/p')
size=$(echo "$line" | sed -n 's/.* \([0-9]*\) of \([0-9]*\) bytes used$/\2/p')
red=$(( $(sed -n 's/^#define KSTACK_RED *(\([0-9]*\) \* 1024).*/\1/p' task.c) * 1024 ))
[ -n "$used" ] && [ -n "$size" ] && [ "$used" -lt "$size" ] &&
    [ "$used" -ge $(( size - red - 1024 )) ]
check "  stopped IN the red zone: ${used:-?} of ${size:-?} bytes, the last $red kept" $?
pc=$(echo "$line" | sed -n 's/.*in the function at \([0-9a-f]*\),.*/\1/p')
fn=$("$ADDR2LINE" -f -e kernel.elf "0x$pc" 2>/dev/null | head -1)
case "$fn" in stack_probe_level|stack_probe_level.*) true ;; *) false ;; esac
check "  the function it names is the one that recursed (${fn:-unresolved})" $?
addrs=$(sed -n '/return addresses/,/panic/p' "$CLEAN" | grep -oE '[0-9a-f]{8}' | head -24)
nres=0
for a in $addrs; do
    f=$("$ADDR2LINE" -f -e kernel.elf "0x$a" 2>/dev/null | head -1)
    case "$f" in stack_probe_level|stack_probe_level.*) nres=$((nres + 1)) ;; esac
done
outer=$(for a in $addrs; do "$ADDR2LINE" -f -e kernel.elf "0x$a" 2>/dev/null | head -1; done | sort -u | tr '\n' ' ')
[ "$nres" -ge 1 ] && grep -qE 'x[0-9]+' <<< "$(sed -n '/return addresses/,/panic/p' "$CLEAN")"
check "  its return addresses resolve to the recursion, written once with a count" $?
case " $outer " in *" syscall_dispatch "*) r=0 ;; *) r=1 ;; esac
check "  and go on out past it, to the system call it came from ($outer)" $r

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
