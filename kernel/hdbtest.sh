#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# hdbtest.sh - a second disk: the slave drive on the IDE channel, hdb.
#
# The machine boots from hda as always, with a second image attached as
# the slave (-drive if=ide,index=1). It must find both, give hdb and its
# partition nodes with Linux's numbers (3:64, 3:65), mount hdb1, read a
# file the host put there and write one of its own. The host then reads
# the second disk with its own tools: e2fsck says clean, and the file
# holds what the machine wrote. A run with no second disk must show
# none, which is every other suite.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-hdbtest.img"; DISK2="$SCRATCH/hdb-hdbtest.img"
OFF=$((2048 * 512))
LOG="$SCRATCH/hdbtest.log"; FIFO="$SCRATCH/hdb.fifo"
rm -f "$LOG"
fsimg()  { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "hdbtest: fsimg $* failed" >&2; exit 1; }; }
fsimg2() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK2" "$@"; }

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh mount umount || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK and $DISK2 ==="
for d in "$DISK" "$DISK2"; do
    rm -f "$d"
    dd if=/dev/zero of="$d" bs=1M count=16 status=none
    printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$d" >/dev/null
done
fsimg mkfs SAGE040 >/dev/null
fsimg2 mkfs SECOND >/dev/null || exit 1
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /mnt /tmp; do fsimg mkdir $d; done
put_shells
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
for p in ls cat; do fsimg put -m 755 ../ports/sbase/bin/$p /bin/$p; done
for p in mount umount; do fsimg put -m 755 ../system/$p /bin/$p; done
echo "from the second disk" > "$SCRATCH/hdb-hello.txt"
fsimg2 put "$SCRATCH/hdb-hello.txt" /hello.txt || exit 1

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide,index=0 \
    -drive file="$DISK2",format=raw,if=ide,index=1 \
    -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {
    local i
    for i in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_for "$1-END [0-9]+" "${3:-600}"; }

wait_for 'kernel ready.*' 300
sleep 1
run START 'echo started'
run LS '/bin/ls -l /dev/hda /dev/hdb /dev/hdb1'
run MOUNT '/bin/mount /dev/hdb1 /mnt'
run MOUNTS '/bin/cat /proc/mounts'
run READ '/bin/cat /mnt/hello.txt'
run WRITE 'echo written by the machine > /mnt/new.txt'
run UMOUNT '/bin/umount /mnt'
printf 'sync; halt\r' >&3
sleep 3
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CL="$SCRATCH/hdb-clean.tmp"
tr -d '\r' < "$LOG" > "$CL"
echo "=== guest session ==="
grep -a '^  disk' "$CL" | sed 's/^/  | /'
sed -n '/started/,$p' "$CL" | sed 's/^/  | /' | head -40
block() { sed -n "/echo \"$1\"-END/,/^$1-END/p" "$CL"; }
status() { sed -n "s/^$1-END //p" "$CL" | tail -1; }

echo "=== checks ==="
grep -aqE "^  disk +: hda '.*', [0-9]+ sectors .*; hdb '.*', 32768 sectors \(16 MiB\), 1 partition$" "$CL"
check "the banner finds both drives: hdb, 16 MiB, one partition" $?
block LS | grep -qE '^b.* 3, +0 .*/dev/hda$' && block LS | grep -qE '^b.* 3, +64 .*/dev/hdb$' &&
    block LS | grep -qE '^b.* 3, +65 .*/dev/hdb1$'
check "/dev/hdb is 3:64 and /dev/hdb1 3:65, as Linux numbers them" $?
[ "$(status MOUNT)" = 0 ] && block MOUNTS | grep -qx '/dev/hdb1 /mnt ext3 rw 0 0'
check "mount /dev/hdb1 /mnt, and /proc/mounts lists it" $?
block READ | grep -qx 'from the second disk'
check "the host's file on the second disk reads" $?
[ "$(status WRITE)" = 0 ] && [ "$(status UMOUNT)" = 0 ]
check "a file written there, and the volume unmounted" $?
PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK2" fsck >/dev/null 2>&1
check "the host's e2fsck finds the second disk clean" $?
[ "$(PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK2" cat /new.txt 2>/dev/null)" = "written by the machine" ]
check "  and the file holds what the machine wrote" $?
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
