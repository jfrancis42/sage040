#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# curstest.sh - terminfo and curses on this machine (ncurses).
#
# ports/ncurses/test/curstest asks the terminfo database about terminals
# that are NOT compiled into the library, so its answers can only have
# come from /usr/share/terminfo, and then drives curses with its output
# going to a file so that the bytes it emits can be read back.
#
# What this script adds, from outside the program:
#
#   - `tput` and `infocmp`, ncurses's own programs, reading the same
#     database from the shell.
#   - THE NEGATIVE CONTROL: the whole run again with the database
#     renamed away. The checks that only the database can answer have to
#     fail then, and the ones the compiled-in fallbacks cover have to
#     keep passing. A test of a database that passes without one is a
#     test of nothing.

set -u

cd "$(dirname "$0")"

. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-curs.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/curstest.log"
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
if ! make -s -C ../ports/ncurses curstest; then
    echo "curstest.sh: could not build ncurses -- see ports/ncurses" >&2
    exit 1
fi
NCOUT=${SAGE_SRC:-$HOME/m68k/src}/build-ncurses-sage040/sage040

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" "$NCOUT/bin/curstest" ::/CURSTEST
mmd -i "$MIMG" ::/BIN
for p in tput infocmp clear; do
    [ -x "$NCOUT/bin/$p" ] && mcopy -o -i "$MIMG" "$NCOUT/bin/$p" "::/BIN/$p"
done
# The database, as ncurses lays it out.
mmd -i "$MIMG" ::/usr ::/usr/share ::/usr/share/terminfo
for d in "$NCOUT"/terminfo/*/; do
    n=$(basename "$d")
    mmd -i "$MIMG" "::/usr/share/terminfo/$n"
    mcopy -o -i "$MIMG" "$d"* "::/usr/share/terminfo/$n/"
done
# ld.so and libc.so, since the program is linked against them.
mmd -i "$MIMG" ::/lib
mcopy -o -i "$MIMG" ../ldso/ld.so ::/lib/ld.so
mcopy -o -i "$MIMG" "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" ::/lib/libc.so

rm -f "$SCRATCH/curs.fifo"
mkfifo "$SCRATCH/curs.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/curs.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/curs.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

printf '/CURSTEST\r' >&3
wait_for "RESULT: " 1800
sleep 0.5

# ncurses's own programs, reading the same database from the shell.
# `tput -Twyse50 clear` cannot be answered by a compiled-in fallback.
printf 'tput -Tvt102 cols\r' >&3
sleep 1
printf 'infocmp -1 wyse50 > /INFO.OUT\r' >&3
sleep 2
printf 'echo TPUT-DONE\r' >&3
wait_for "TPUT-DONE"
printf 'echo STILL-HERE\r' >&3
wait_for "STILL-HERE"
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/curs.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/curs-clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/curs-clean.tmp"

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
done < <(grep -E "^  \[( OK |FAIL)\] " "$SCRATCH/curs-clean.tmp")

grep -qx "RESULT: PASS" "$SCRATCH/curs-clean.tmp"
check "curstest ran to the end" $?

echo "=== checks: ncurses's own programs ==="

grep -qx "80" "$SCRATCH/curs-clean.tmp"
check "tput -Tvt102 cols says 80" $?

mtype -i "$MIMG" ::/INFO.OUT 2>/dev/null | tr -d '\r' > "$SCRATCH/infocmp.tmp"
grep -q "wyse50|" "$SCRATCH/infocmp.tmp"
check "infocmp printed the wyse50 entry -- from the database" $?
grep -qE '^\s+cup=' "$SCRATCH/infocmp.tmp"
check "  with its capabilities in it" $?

! grep -qE "panic|exception at|DOUBLE MMU" "$SCRATCH/curs-clean.tmp"
check "no panic, no kernel exception" $?

grep -qx "STILL-HERE" "$SCRATCH/curs-clean.tmp"
check "the shell is still there afterwards" $?

# --- the negative control --------------------------------------------
#
# Rename the database away and run the program again. The checks that
# the compiled-in fallbacks can answer have to keep passing; the ones
# that only the database can answer have to fail. If everything still
# passes, this suite was never testing a database at all.
echo "=== the control: the same program with no database ==="
LOG2="$SCRATCH/curstest-nodb.log"
rm -f "$LOG2"
mmove -i "$MIMG" ::/usr/share/terminfo ::/usr/share/terminfo.off

rm -f "$SCRATCH/curs.fifo"
mkfifo "$SCRATCH/curs.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/curs.fifo" > "$LOG2" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/curs.fifo"
sleep "$BOOT_WAIT"
printf '/CURSTEST\r' >&3
for _ in $(seq 1 1800); do
    grep -qF "RESULT: " "$LOG2" 2>/dev/null && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.2
done
sleep 0.3
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/curs.fifo"
mmove -i "$MIMG" ::/usr/share/terminfo.off ::/usr/share/terminfo
tr -d '\r' < "$LOG2" > "$SCRATCH/curs-nodb.tmp"

grep -q "^  \[ OK \] setupterm(vt102)" "$SCRATCH/curs-nodb.tmp"
check "without the database, the compiled-in vt102 still works" $?
grep -q "^  \[FAIL\] setupterm(wyse50)" "$SCRATCH/curs-nodb.tmp"
check "  but wyse50 does not -- so the database is what answered before" $?
grep -q "^  \[FAIL\] setupterm(xterm)" "$SCRATCH/curs-nodb.tmp"
check "  nor xterm" $?
grep -qx "RESULT: FAIL" "$SCRATCH/curs-nodb.tmp"
check "  and the control run fails, as a control must" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
