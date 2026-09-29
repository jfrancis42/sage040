#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# tmpfstest.sh - /tmp and /dev/shm, in memory.
#
# libc/test/tmpfstest checks files, directories, links, the working
# directory, shm_open and the sticky bit in tmpfs. What makes it more
# than a filesystem test is the disk: a file is planted in the DISK's
# /tmp before boot, which tmpfs must hide, and afterwards the host looks
# in the disk's /tmp for what the machine wrote there and must not find
# it -- "not on the disk", seen from both sides by something that is not
# tmpfs.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-tmpfs.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/tmpfstest.log"
FIFO="$SCRATCH/tmpfs.fifo"
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
make -s -C ../libc/test tmpfstest || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /tmp
fsimg put -m 755 ../libc/test/tmpfstest /tmpfstest
echo "on the disk" > "$SCRATCH/ondisk.txt"
fsimg put "$SCRATCH/ondisk.txt" /tmp/ondisk.txt

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
printf 'echo TMPFS-START\r' >&3
printf '/tmpfstest; echo "TMPFS-EXIT $?"\r' >&3
wait_for "TMPFS-EXIT [0-9]+" 900
printf 'sync; echo "SYNC-DONE"\r' >&3
wait_for "SYNC-DONE" 300
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"

tr -d '\r' < "$LOG" > "$SCRATCH/tmpfs-clean.tmp"
echo "=== guest session ==="
sed -n '/TMPFS-START/,$p' "$SCRATCH/tmpfs-clean.tmp" | sed 's/^/  | /'

echo "=== checks: on the machine ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*) check "${line#  ok   }" 0 ;;
        "  FAIL "*) check "${line#  FAIL }" 1 ;;
    esac
done < <(sed -n '/^TMPFS-START/,/^TMPFS-EXIT/p' "$SCRATCH/tmpfs-clean.tmp")
grep -qE '^tmpfstest: [0-9]+ checks, 0 failed$' "$SCRATCH/tmpfs-clean.tmp"
check "ran to the end, nothing failed" $?

echo "=== checks: on the host ==="
ls_tmp=$(fsimg ls /tmp 2>/dev/null)
grep -qw ondisk.txt <<< "$ls_tmp"
check "the disk's /tmp still has what the host put there" $?
! grep -qwE 'never-on-disk.txt|a.txt|rel.txt' <<< "$ls_tmp"
check "and nothing the machine wrote to /tmp is on the disk" $?

grep -qE 'panic|bus error|address error' "$SCRATCH/tmpfs-clean.tmp"
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
