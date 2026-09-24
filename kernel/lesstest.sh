#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# lesstest.sh - less, and a full-screen program on this terminal.
#
# less is the first program here that uses the terminal the way a
# full-screen program does: raw mode, the screen's size, and the
# terminal's own capabilities looked up in the terminfo database rather
# than assumed. So this suite is as much about the terminal as about
# less.
#
# It drives the pager the way a person does -- keys in, screens out --
# and checks what came back on the serial line. The negative control is
# the same run with TERM set to a terminal that cannot address its
# cursor (dumb), where less must fall back rather than send escape
# sequences that would do nothing.

set -u

cd "$(dirname "$0")"

. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-less.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/lesstest.log"
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
make -s -C ../ports/less || exit 1
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
NCOUT=$SRCDIR/build-ncurses-sage040/sage040
LOUT=$SRCDIR/build-less-sage040/sage040

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib
fsimg put "$LOUT/bin/less" /bin/less
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../ports/sbase/bin/wc /bin/wc
# The terminfo database, which is where less learns what a vt102 and a
# dumb terminal can do.
fsimg mkdir /usr; fsimg mkdir /usr/share; fsimg mkdir /usr/share/terminfo
for d in "$NCOUT"/terminfo/*/; do
    n=$(basename "$d")
    fsimg mkdir "/usr/share/terminfo/$n"
    fsimg put "$d"* "/usr/share/terminfo/$n/"
done
# A file with numbered lines, so that which screenful is showing is
# visible in the output rather than a matter of counting.
: > "$SCRATCH/lines.tmp"
for i in $(seq 1 200); do
    printf 'line-%03d the quick brown fox jumps over the lazy dog\n' "$i" \
        >> "$SCRATCH/lines.tmp"
done
fsimg put "$SCRATCH/lines.tmp" /LINES.TXT

rm -f "$SCRATCH/less.fifo"
mkfifo "$SCRATCH/less.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/less.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/less.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

# less on a vt102: the first screenful, then a page forward, then a
# jump to the end, then quit. The terminal is what the console says it
# is; TERM is vt102 because that is what this machine's console is.
printf 'less /LINES.TXT\r' >&3
sleep 3
printf ' ' >&3                  # space: the next screenful
sleep 2
printf 'G' >&3                  # G: the end of the file
sleep 2
printf 'q' >&3                  # q: quit
sleep 2
printf 'echo LESS-DONE\r' >&3
wait_for "LESS-DONE"

# What less prints when it is not a terminal at all: a pipe, where it
# behaves as cat does.
printf 'less /LINES.TXT | wc -l > /PIPED.TXT\r' >&3
sleep 4
printf 'echo PIPE-DONE\r' >&3
wait_for "PIPE-DONE"

# And on a terminal that cannot address its cursor. less warns and
# waits for RETURN before it will use one -- so that is what it gets.
printf 'export TERM=dumb\r' >&3
sleep 1
printf 'less /LINES.TXT\r' >&3
sleep 3
printf '\r' >&3                 # RETURN: yes, carry on anyway
sleep 3
printf 'q' >&3
sleep 2
printf 'export TERM=vt102\r' >&3
printf 'echo DUMB-DONE\r' >&3
wait_for "DUMB-DONE"
printf 'echo STILL-HERE\r' >&3
wait_for "STILL-HERE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/less.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/less-clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/less-clean.tmp"

echo "=== checks: less on a vt102 ==="

grep -q "line-001" "$SCRATCH/less-clean.tmp"
check "the first screenful is the top of the file" $?

grep -q "line-020" "$SCRATCH/less-clean.tmp"
check "  a whole screen of it, not one line" $?

grep -q "line-040" "$SCRATCH/less-clean.tmp"
check "space moved on to the next screenful" $?

grep -q "line-200" "$SCRATCH/less-clean.tmp"
check "G jumped to the end of the file" $?

grep -qE $'\033\[' "$SCRATCH/less-clean.tmp"
check "  and it addressed the cursor to do it (escape sequences)" $?

grep -q "LESS-DONE" "$SCRATCH/less-clean.tmp"
check "q quit, and the shell came back" $?

echo "=== checks: not a terminal ==="

fsimg cat /PIPED.TXT 2>/dev/null | tr -d '\r' > "$SCRATCH/piped.tmp"
grep -qE '^ *200$' "$SCRATCH/piped.tmp"
check "into a pipe, less is cat: all 200 lines went through" $?

echo "=== checks: a terminal that cannot address its cursor ==="

# Everything after TERM=dumb, which is where the control lives.
sed -n '/export TERM=dumb/,/DUMB-DONE/p' "$SCRATCH/less-clean.tmp" \
    > "$SCRATCH/less-dumb.tmp"

grep -q "terminal is not fully functional" "$SCRATCH/less-dumb.tmp"
check "less says so rather than sending sequences that would do nothing" $?

grep -q "line-001" "$SCRATCH/less-dumb.tmp"
check "  and shows the file anyway once told to carry on" $?

! grep -qE $'\033\[[0-9;]*[Hf]' "$SCRATCH/less-dumb.tmp"
check "  and sends no cursor addressing, having asked terminfo" $?

! grep -qE "panic|exception at|DOUBLE MMU" "$SCRATCH/less-clean.tmp"
check "no panic, no kernel exception" $?

grep -qx "STILL-HERE" "$SCRATCH/less-clean.tmp"
check "the shell is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
