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

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

DISK=hd-vm.img
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG=vmtest.log
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
make -s -C ../user || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" ../user/faulter ::/FAULTER
mcopy -o -i "$MIMG" ../user/hello ::/HELLO

: > session.tmp
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
    printf 'echo SHELL-SURVIVED\r'
} >> session.tmp

rm -f in.fifo
mkfifo in.fifo

"$QEMU" -M sage040 -cpu m68040 -m 4 \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < in.fifo > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> in.fifo
sleep "$BOOT_WAIT"
cat session.tmp >&3

for _ in $(seq 1 200); do
    if grep -qF "SHELL-SURVIVED" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
    sleep 0.2
done

sleep 0.5
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f in.fifo

tr -d '\r' < "$LOG" > clean.tmp

echo "=== guest session ==="
sed 's/^/  | /' clean.tmp

echo "=== checks: the MMU is on ==="

grep -q "mmu     : on," clean.tmp
check "the kernel reports the MMU enabled" $?

grep -q "kernel ready." clean.tmp
check "and the machine came all the way up with it on" $?

echo "=== checks: a program can use its own memory ==="

grep -qx "OWN-MEMORY-OK" clean.tmp
check "a program reads and writes its own memory" $?

echo "=== checks: and nothing else ==="

# Every forbidden access must have faulted. If protection were off, the
# program would print NOT-PROTECTED and carry on -- so the check is that
# the string never appears anywhere.
! grep -q "NOT-PROTECTED" clean.tmp
check "no forbidden access was allowed" $?

for what in "00000400:the kernel's own text" \
            "00000000:the vector table" \
            "ff000000:the device registers" \
            "f0000000:video memory" \
            "10100000:the unmapped gap below its stack" \
            "40000000:an address in no map at all"; do
    addr=${what%%:*}
    desc=${what#*:}
    grep -q "bus error at 0x$addr" clean.tmp
    check "a program is refused $desc" $?
done

test "$(grep -c 'segmentation fault' clean.tmp)" -ge 6
check "each refusal killed the program, and the shell said why" $?

echo "=== checks: a bad pointer to the kernel is an error, not a crash ==="

grep -qx "BADPTR-ALL-REFUSED" clean.tmp
check "write, open, uname and read all refused an unmapped pointer" $?

test "$(grep -c '= 14$' clean.tmp)" -ge 4
check "  and every one of them returned EFAULT" $?

echo "=== checks: the machine is still standing ==="

grep -q "argv\[1\] = after-the-faults" clean.tmp
check "a program still runs after all of that" $?

grep -qx "SHELL-SURVIVED" clean.tmp
check "the shell survived every fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
