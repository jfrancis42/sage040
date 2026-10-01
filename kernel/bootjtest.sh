#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# bootjtest.sh - the boot ROM reads a journal that still needs replaying.
#
# The ROM runs before the kernel, so before anything has replayed the
# log. A KERNEL.ROM replaced in the seconds before a power cut has its
# new directory entry and inode in the log and NOT at home; a ROM that
# reads only home blocks boots the old kernel (or, after enough churn,
# garbage). bootrom.c builds a map from the log of what is newer there,
# read only, and reads through it.
#
# The test makes exactly that disk: /K2 renamed over /KERNEL.ROM, the
# machine stopped after the commit block of the sync that followed
# (kstat KSTAT_JOURNAL_STOP, as journaltest does). Then:
#   - the HOST, reading home blocks only (debugfs, no replay), finds the
#     OLD inode under the name -- the control: the map is needed;
#   - the ROM reports reading the log and loads the NEW inode;
#   - the kernel it started replays the log, and e2fsck finds the volume
#     clean, with the name on the new inode.

set -u
cd "$(dirname "$0")"
. ../machine.conf

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-bootj.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/bootjtest.log"; FIFO="$SCRATCH/bootj.fifo"
rm -f "$LOG" "$LOG.1" "$LOG.2"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "bootjtest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../libc/test jtest >/dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg put kernel.rom /K2
fsimg mkdir /lib; fsimg mkdir /tmp
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../libc/test/jtest /jtest
ino_of() { debugfs -R "stat $1" "$DISK?offset=$OFF" 2>/dev/null |
           sed -n 's/^Inode: \([0-9]*\) .*/\1/p'; }
OLD=$(ino_of /KERNEL.ROM); NEW=$(ino_of /K2)

qpid=
boot() {                        # boot LOGFILE
    rm -f "$FIFO"; mkfifo "$FIFO"
    "$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
        -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
        -chardev stdio,id=con,signal=off -serial chardev:con \
        < "$FIFO" > "$1" 2>&1 &
    qpid=$!
    exec 3> "$FIFO"
}
wait_for() {                    # wait_for LOGFILE REGEX [TENTHS]
    local i
    for i in $(seq 1 "${3:-600}"); do
        [ "$(tr -d '\r' < "$1" | grep -acxE -- "$2")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
stop() { exec 3>&-; kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null; rm -f "$FIFO"; }

# --- 1: the rename, and the power cut after its commit ---------------
boot "$LOG.1"
wait_for "$LOG.1" 'kernel ready.*' 600
sleep 0.5
printf '/jtest sync; /jtest stop 257 1; mv /K2 /KERNEL.ROM; /jtest sync\r' >&3
wait_for "$LOG.1" 'journal: stopped after the commit block' 600
check "the machine stops after the commit block of the rename" $?
stop

echo "=== checks ==="
[ -n "$OLD" ] && [ -n "$NEW" ] && [ "$OLD" != "$NEW" ] && [ "$(ino_of /KERNEL.ROM)" = "$OLD" ]
check "read from home blocks only, the name is still the OLD inode ($OLD, not $NEW): the log is needed" $?

# --- 2: the ROM, through the log --------------------------------------
boot "$LOG.2"
wait_for "$LOG.2" 'kernel ready.*' 600
r=$?
CL="$SCRATCH/bootj-clean.tmp"; tr -d '\r' < "$LOG.2" > "$CL"
echo "=== the second boot ==="
sed -n '1,/kernel ready/p' "$CL" | grep -aE 'journal|KERNEL.ROM|partition' | sed 's/^/  | /'
grep -aqE '^the journal needs replaying: [1-9][0-9]* transactions, [1-9][0-9]* blocks read from the log' "$CL"
check "the ROM reads the log of a volume that needs recovery" $?
grep -aqE "^KERNEL.ROM +[0-9]+ bytes, inode $NEW\$" "$CL"
check "  and loads the NEW KERNEL.ROM, inode $NEW" $?
[ $r -eq 0 ]
check "  which boots" $?
printf 'halt\r' >&3
wait_for "$LOG.2" 'halting.*' 300
sleep 1
stop

p="$SCRATCH/bootj.part"
dd if="$DISK" of="$p" bs=512 skip=2048 status=none
e2fsck -fn "$p" > "$SCRATCH/bootj.fsck" 2>&1 &&
    ! grep -qE 'Fix\?|count wrong|differences|Unconnected|Unattached|recover' "$SCRATCH/bootj.fsck"
check "the kernel replayed it: e2fsck finds the volume clean" $?
[ "$(ino_of /KERNEL.ROM)" = "$NEW" ] && [ -z "$(ino_of /K2)" ]
check "  with KERNEL.ROM on the new inode and /K2 gone" $?
cat "$LOG.1" "$LOG.2" > "$LOG"

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
