#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# apitest.sh - the system call surface a ported program expects.
#
# The other suites test subsystems: the filesystem, memory protection,
# the editor, the network. This one tests the ABI itself -- the calls
# that exist so that software written for a Unix will build and run
# here, and that are individually dull and collectively the difference
# between a machine that can host other people's programs and one that
# cannot.
#
# It grows as that surface does. Each group below is a task from
# progress.md, and a group is only added once its calls actually work,
# so a failure here is always a regression rather than a thing not
# written yet.
#
# The checks are made by PROGRAMS in apps/, not by the shell, because
# the point is that a program can do these things. The program reports
# `ok` or `FAIL` per line and this script counts them -- so a new check
# is a line of C rather than a line of shell.

set -u

cd "$(dirname "$0")"

# How big the machine is. One place, shared with the Makefiles.
. "$(dirname "$0")/../machine.conf"

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-api.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/apitest.log"
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
mcopy -o -i "$MIMG" ../apps/statfs ::/STATFS
mcopy -o -i "$MIMG" ../apps/cdtest ::/CDTEST
mcopy -o -i "$MIMG" ../apps/hello ::/HELLO
mcopy -o -i "$MIMG" ../apps/memtest ::/MEMTEST
mmd -i "$MIMG" ::/ETC
mmd -i "$MIMG" ::/BIN
mcopy -o -i "$MIMG" ../system/env ::/BIN/ENV
printf 'echo rc-ran\r\n' > "$SCRATCH/rc.tmp"
mcopy -o -i "$MIMG" "$SCRATCH/rc.tmp" ::/ETC/RC

: > "$SCRATCH/session.tmp"
{
    # --- descriptors: fstat, access, dup, isatty ---
    printf 'statfs /ETC/RC\r';          sleep 2

    # --- the working directory belongs to the task ---
    printf 'pwd\r';                     sleep 1
    printf 'cdtest /BIN\r';             sleep 2
    printf 'pwd\r';                     sleep 1

    # --- an absolute path means the same thing from anywhere ---
    printf 'cd /ETC\r';                 sleep 1
    printf 'cat /ETC/RC\r';             sleep 1
    printf 'stat /BIN/ENV\r';           sleep 1
    printf '/BIN/ENV\r';                sleep 1
    printf 'cd /\r';                    sleep 1

    # --- memory: brk, sbrk, mmap, munmap, mprotect ---
    printf 'echo FREE-BEFORE\r';        sleep 0.5
    printf 'free\r';                    sleep 0.5
    printf 'memtest\r';                 sleep 4
    printf 'echo FREE-AFTER\r';         sleep 0.5
    printf 'free\r';                    sleep 0.5
    printf 'memtest past\r';            sleep 2
    printf 'memtest unmapped\r';        sleep 2
    printf 'memtest readonly\r';        sleep 2
    printf 'memtest none\r';            sleep 2

    printf 'echo SHELL-SURVIVED\r'
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
C="$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$C"

# ---------------------------------------------------------------
# The program's own checks. Each `ok` line it printed is a pass and
# each `FAIL` line is a failure, reported here under its own name so
# that a break says which call stopped working.
# ---------------------------------------------------------------
echo "=== checks: the programs' own (descriptors, brk, mmap, mprotect) ==="

grep -q "statfs: done" "$C"
check "statfs ran to the end" $?

grep -q "memtest: done" "$C"
check "memtest ran to the end" $?

test "$(grep -c '^  FAIL ' "$C")" -eq 0
check "  and every check inside them passed" $?

# Named individually, so a regression says which one.
while IFS= read -r line; do
    what=${line#  ok   }
    what=${what#  FAIL }
    case "$line" in
        "  ok   "*)   check "$what" 0 ;;
        "  FAIL "*)   check "$what" 1 ;;
    esac
done < <(grep -E '^  (ok|FAIL) ' "$C")

echo "=== checks: the working directory belongs to the task ==="

grep -q "cdtest: now in /BIN" "$C"
check "a program can chdir itself somewhere" $?

# The shell printed pwd twice, before and after. Both must say "/".
test "$(grep -c '^/$' "$C")" -ge 2
check "  and the shell that started it did NOT move" $?

echo "=== checks: an absolute path is absolute ==="

grep -q "rc-ran" "$C"
check "/etc/rc ran at startup" $?

# cat /ETC/RC issued from inside /ETC. vfs.c used to strip the leading
# slash, so it resolved relative to the cwd and became /ETC/ETC/RC.
test "$(grep -c 'echo rc-ran' "$C")" -ge 1
check "cat of an absolute path worked from inside that directory" $?

grep -q "PATH=" "$C"
check "a program ran by absolute path from another directory" $?

echo "=== checks: memory the heap gave back is gone ==="

grep -q "memtest: touching memory the heap gave back" "$C"
check "memtest shrank its heap and reached past the end" $?

! grep -q "NOT-PROTECTED" "$C"
check "  and the access was refused" $?

test "$(grep -c "^memtest: segmentation fault" "$C")" -eq 4
check "  and the program was killed for it" $?

for mode in unmapped readonly none; do
    grep -q "memtest: abusing a $mode page" "$C"
    check "memtest touched a $mode page" $?
done
check "  and all four were killed, none got through" \
    "$(grep -c "NOT-PROTECTED" "$C")"
grep -q "memtest: read-only page still reads" "$C"
check "a read-only page was readable up to the write" $?

echo "=== checks: exit gives back every page, mapped or not ==="

used_after() {
    awk -v m="$1" '$0 == m { f = 1 } f && $1 == "used" { print $2; exit }' "$C"
}
before=$(used_after FREE-BEFORE)
after=$(used_after FREE-AFTER)
echo "  used pages: before=${before:-?} after=${after:-?}"
test -n "$before" && test -n "$after" && [ "$before" -eq "$after" ]
check "memtest left mappings, PROT_NONE pages and a file behind, and none leaked" $?

echo "=== checks: nothing broke ==="

grep -q "SHELL-SURVIVED" "$C"
check "the shell survived all of it" $?

! grep -qE "exception|panic|DOUBLE" "$C"
check "no faults anywhere" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
[ "$fail" -eq 0 ]
