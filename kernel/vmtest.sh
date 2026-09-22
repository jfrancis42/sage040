#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# vmtest.sh - does the memory protection actually protect anything?
#
# The MMU being on proves nothing by itself. What has to be demonstrated
# is that the accesses a program must not be able to make actually fail,
# that failing kills the program and not the machine, and that a bad
# pointer handed to the KERNEL comes back as an error rather than taking
# the system down with it.
#
# That last one is the important half. A program faulting on its own is
# the program's problem. A program able to fault the kernel by passing it
# rubbish would mean any program could stop the machine at will, which is
# not memory protection at all -- it is memory protection with a hole in
# it, and the hole is the part worth testing.
#
# Every check is run from the shell, which must still be there at the end
# of all of them. Runs on a scratch image.

set -u

cd "$(dirname "$0")"

# How big the machine is. One place, shared with the Makefiles.
. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

#
# Everything this test writes goes in one place.
#
# Scratch disk images are 16 MB each and there is one per test suite, so
# leaving them beside the source meant 67 MB of build product scattered
# through the tree with names that looked like part of it. They are all
# under scratch/ now, which `make clean` removes and git ignores.
#
#
# Computed AFTER the cd above, from the working directory rather than
# from $0 -- which has already been used once and is relative to where
# the script was invoked from, not to where it now is. Deriving it from
# $0 a second time worked when the script was run as ./edittest.sh and
# failed when it was run by path, which is a difference nobody should
# have to notice.
#
SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-vm.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/vmtest.log"
# Gone before QEMU starts, so a run that never reaches the guest has no
# log to grade -- rather than silently grading the last run's.
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
mcopy -o -i "$MIMG" ../apps/faulter ::/FAULTER
mcopy -o -i "$MIMG" ../apps/hello ::/HELLO
mcopy -o -i "$MIMG" ../apps/spin ::/SPIN

: > "$SCRATCH/session.tmp"
{
    printf 'faulter ok\r';       sleep 1
    printf 'faulter kernel\r';   sleep 1
    printf 'faulter vectors\r';  sleep 1
    printf 'faulter device\r';   sleep 1
    printf 'faulter video\r';    sleep 1
    printf 'faulter gap\r';      sleep 1
    printf 'faulter wild\r';     sleep 1
    printf 'faulter badptr\r';   sleep 1
    # The machine is still usable after all of that.
    printf 'hello after-the-faults\r'; sleep 1
    # A killed background job must give its memory back without anybody
    # running `jobs`. The pid is read from `ps` below, not assumed.
    printf 'echo FREE-BEFORE\r';  sleep 0.5
    printf 'free\r';              sleep 0.5
    printf 'spin &\r';            sleep 1
    printf 'echo FREE-DURING\r';  sleep 0.5
    printf 'free\r';              sleep 0.5
    printf 'ps\r';                sleep 1
} >> "$SCRATCH/session.tmp"

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
cat "$SCRATCH/session.tmp" >&3

# The second half needs spin's pid, which only `ps` knows.
spin_pid=
for _ in $(seq 1 100); do
    spin_pid=$(tr -d '\r' < "$LOG" |
               awk '$1 ~ /^[0-9]+$/ && $NF == "spin" { print $1; exit }')
    [ -n "$spin_pid" ] && break
    sleep 0.2
done
{
    printf 'kill -9 %s\r' "${spin_pid:-0}"; sleep 1
    # An empty line is a fresh prompt, which is where the reap happens.
    printf '\r';                  sleep 0.5
    printf 'echo FREE-AFTER\r';   sleep 0.5
    printf 'free\r';              sleep 0.5
    printf 'echo SHELL-SURVIVED\r'
} >&3

for _ in $(seq 1 200); do
    if grep -qF "SHELL-SURVIVED" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
    sleep 0.2
done

sleep 0.5
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/clean.tmp"

echo "=== checks: the MMU is on ==="

grep -q "mmu     : on," "$SCRATCH/clean.tmp"
check "the kernel reports the MMU enabled" $?

grep -q "kernel ready." "$SCRATCH/clean.tmp"
check "and the machine came all the way up with it on" $?

echo "=== checks: a program can use its own memory ==="

grep -qx "OWN-MEMORY-OK" "$SCRATCH/clean.tmp"
check "a program reads and writes its own memory" $?

echo "=== checks: and nothing else ==="

# Every forbidden access must have faulted. If protection were off, the
# program would print NOT-PROTECTED and carry on -- so the check is that
# the string never appears anywhere.
! grep -q "NOT-PROTECTED" "$SCRATCH/clean.tmp"
check "no forbidden access was allowed" $?

for what in "00000400:the kernel's own text" \
            "00000000:the vector table" \
            "ff000000:the device registers" \
            "f0000000:video memory" \
            "10100000:the unmapped gap below its stack" \
            "40000000:an address in no map at all"; do
    addr=${what%%:*}
    desc=${what#*:}
    grep -q "bus error at 0x$addr" "$SCRATCH/clean.tmp"
    check "a program is refused $desc" $?
done

test "$(grep -c 'segmentation fault' "$SCRATCH/clean.tmp")" -ge 6
check "each refusal killed the program, and the shell said why" $?

echo "=== checks: a bad pointer to the kernel is an error, not a crash ==="

grep -qx "BADPTR-ALL-REFUSED" "$SCRATCH/clean.tmp"
check "write, open, uname and read all refused an unmapped pointer" $?

test "$(grep -c '= 14$' "$SCRATCH/clean.tmp")" -ge 4
check "  and every one of them returned EFAULT" $?

echo "=== checks: a killed background job gives its memory back ==="

# The pages in use, from the first `free` after a marker line.
used_after() {
    awk -v m="$1" '$0 == m { f = 1 } f && $1 == "used" { print $2; exit }' \
        "$SCRATCH/clean.tmp"
}
before=$(used_after FREE-BEFORE)
during=$(used_after FREE-DURING)
after=$(used_after FREE-AFTER)
echo "  used pages: before=${before:-?} during=${during:-?} after=${after:-?}"

test -n "$spin_pid"
check "ps listed the background job" $?

# 256 pages of stack alone, so anything under 200 means spin never ran.
test -n "$before" && test -n "$during" && [ $((during - before)) -ge 200 ]
check "a running program holds its stack (~1 MB)" $?

# A few pages of slack for whatever the shell itself allocated.
test -n "$after" && [ $((after - before)) -le 4 ] && [ $((after - before)) -ge -4 ]
check "killing it and returning to the prompt freed all of it" $?

echo "=== checks: the machine is still standing ==="

grep -q "argv\[1\] = after-the-faults" "$SCRATCH/clean.tmp"
check "a program still runs after all of that" $?

grep -qx "SHELL-SURVIVED" "$SCRATCH/clean.tmp"
check "the shell survived every fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
