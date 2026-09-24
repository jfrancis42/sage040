#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fscktest.sh - checking and repairing the disk, and knowing when to.
#
# An ext2 image is damaged on the host, in each of the ways
# ext2damage.py knows -- blocks nothing reaches, a block claimed twice,
# a pointer past the end of the volume, a link count that is not the
# number of names, free counts that are a lie, an inode no name
# reaches, a wrong ".." -- and the host's own e2fsck is asked FIRST, to
# prove the damage is real. Then the guest repairs it, and e2fsck is
# asked again.
#
# ext2damage.py shares no code with the kernel's driver and none with
# e2fsprogs: it writes the raw image with struct. Damage built out of
# the thing under test can only express faults that thing can make.
#
# Three sessions:
#   1. damaged, and flagged as not cleanly unmounted: the kernel checks
#      and repairs it at boot, before anything uses it;
#   2. damaged but flagged clean: nothing happens at boot, and /bin/fsck
#      reports it (exit 4), fsck -y repairs it (1), fsck is then clean (0);
#   3. the flag itself: killing the emulator leaves the volume dirty, the
#      next boot checks it, and shutdown leaves it clean.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"

. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-fsck.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/fscktest.log"
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
make -s -C ../system || exit 1

# A fresh image with something on it to damage.
make_image() {
    rm -f "$DISK"
    dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
    printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
        | sfdisk -q "$DISK" >/dev/null
    fsimg mkfs SAGE040
    fsimg put kernel.rom /KERNEL.ROM
    fsimg mkdir /bin; fsimg mkdir /SUB
    fsimg put -m 755 ../system/fsck /bin/fsck
    fsimg put -m 755 ../system/shutdown /bin/shutdown
    head -c 40000 /dev/urandom > "$SCRATCH/a.tmp"
    head -c 20000 /dev/urandom > "$SCRATCH/b.tmp"
    head -c 20000 /dev/urandom > "$SCRATCH/c.tmp"
    head -c 30000 /dev/urandom > "$SCRATCH/keep.tmp"
    echo small > "$SCRATCH/d.tmp"
    for f in a b c d keep; do
        fsimg put "$SCRATCH/$f.tmp" "/$(echo $f | tr a-z A-Z).TXT"
    done
    LC_ALL=C.UTF-8 fsimg put "$SCRATCH/d.tmp" "/Long Name For Orphan.txt"
    fsimg put "$SCRATCH/d.tmp" /SUB/X.TXT
}

damage() {
    local d=./ext2damage.py
    python3 $d "$DISK" "$OFFSET" lost A.TXT
    python3 $d "$DISK" "$OFFSET" cross B.TXT C.TXT
    python3 $d "$DISK" "$OFFSET" badblock D.TXT
    python3 $d "$DISK" "$OFFSET" links SUB 9
    python3 $d "$DISK" "$OFFSET" unattached
    python3 $d "$DISK" "$OFFSET" dotdot SUB
    python3 $d "$DISK" "$OFFSET" counts
}

host_fsck() {
    fsimg fsck > "$SCRATCH/hostfsck.tmp" 2>&1
    local rc=$?
    sed 's/^/  | host: /' "$SCRATCH/hostfsck.tmp"
    return $rc
}

# The one flag ext2 keeps: s_state in the superblock, bit 0 set when the
# volume was unmounted cleanly. Read off the disk with struct, not asked
# of a tool that might be reading the same field wrongly.
flags() {
    python3 - "$DISK" "$OFFSET" <<'SBEOF'
import struct, sys
f = open(sys.argv[1], "rb")
f.seek(int(sys.argv[2]) + 1024)
sb = f.read(1024)
print("clean" if struct.unpack_from("<H", sb, 58)[0] & 1 else "dirty")
SBEOF
}

wait_for() {
    for _ in $(seq 1 300); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

start_qemu() {
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
}

stop_qemu() {
    exec 3>&-
    for _ in $(seq 1 50); do
        kill -0 "$qemu_pid" 2>/dev/null || break
        sleep 0.1
    done
    kill "$qemu_pid" 2>/dev/null
    wait "$qemu_pid" 2>/dev/null
    rm -f "$SCRATCH/in.fifo"
    tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"
    sed 's/^/  | /' "$SCRATCH/clean.tmp"
}

echo "=== session 1: damaged, and not cleanly unmounted ==="
make_image
damage
python3 ./ext2damage.py "$DISK" "$OFFSET" dirty
host_fsck
test $? -ne 0
check "the host's e2fsck finds the damage before the guest sees it" $?
grep -qi "multiply-claimed\|claimed by more than one" "$SCRATCH/hostfsck.tmp" &&
    grep -qi "illegal block" "$SCRATCH/hostfsck.tmp" &&
    grep -qi "ref count is 9" "$SCRATCH/hostfsck.tmp"
check "  including the shared block, the illegal pointer and the link count" $?

start_qemu
printf 'fsck\r' >&3;            sleep 2
printf 'echo FSCK-RC=$?\r' >&3; sleep 0.5
printf 'halt\r' >&3
wait_for "halting"
sleep 0.5
stop_qemu

grep -q "fsck.*not cleanly unmounted; checked .* [1-9][0-9]* problems, [1-9][0-9]* repairs" "$SCRATCH/clean.tmp"
check "at boot, the kernel saw the volume was dirty and repaired it" $?
grep -qx "fsck: clean" "$SCRATCH/clean.tmp" && grep -qx "FSCK-RC=0" "$SCRATCH/clean.tmp"
check "  and /bin/fsck then finds it clean, exit status 0" $?
host_fsck
check "the host's e2fsck agrees it is clean" $?
test "$(flags)" = clean
check "halt left the volume marked cleanly unmounted" $?
LC_ALL=C fsimg cat /KEEP.TXT > "$SCRATCH/keep.out" 2>/dev/null
cmp -s "$SCRATCH/keep.tmp" "$SCRATCH/keep.out"
check "a file nothing damaged is byte for byte what it was" $?
LC_ALL=C fsimg cat /SUB/X.TXT 2>/dev/null | grep -qx small
check "and so is one in the directory whose .. was repaired" $?

echo "=== session 2: damaged, flagged clean ==="
make_image
damage
start_qemu
printf 'echo REPORT-START\r' >&3;   sleep 0.3
printf 'fsck\r' >&3;                sleep 2
printf 'echo FSCK-RC=$?\r' >&3;     sleep 0.5
printf 'fsck -y\r' >&3;             sleep 2
printf 'echo FSCK-RC=$?\r' >&3;     sleep 0.5
printf 'fsck\r' >&3;                sleep 2
printf 'echo FSCK-RC=$?\r' >&3;     sleep 0.5
printf 'halt\r' >&3
wait_for "halting"
sleep 0.5
stop_qemu

! grep -q "fsck.*not cleanly unmounted" "$SCRATCH/clean.tmp"
check "a volume flagged clean is not checked at boot" $?
for what in "pointers out of range or not free" \
            "blocks shared by two files, or by a loop" \
            "wrong . or .. entries" \
            "blocks nothing reaches" \
            "link counts unequal to the names found" \
            "in-use inodes no name reaches" \
            "free counts unequal to the bitmaps"; do
    grep -q "^  [0-9]* $what" "$SCRATCH/clean.tmp"
    check "fsck reports: $what" $?
done
test "$(grep -o 'FSCK-RC=[0-9][0-9]*' "$SCRATCH/clean.tmp" | tr '\n' ' ')" = \
     "FSCK-RC=4 FSCK-RC=1 FSCK-RC=0 "
check "exit status 4 unrepaired, 1 after -y, 0 when clean" $?
host_fsck
check "the host's e2fsck agrees fsck -y repaired it" $?

echo "=== session 3: the flag itself ==="
make_image
start_qemu
printf 'echo written > /NOTE.TXT\r' >&3
sleep 1
kill "$qemu_pid" 2>/dev/null      # no halt: as a crash or a reset would
stop_qemu >/dev/null
test "$(flags)" = dirty
check "a machine stopped without unmounting leaves the volume dirty" $?

fsimg ls / >/dev/null 2>&1
check "  and the host's tools can still read it" $?

start_qemu
printf '/bin/shutdown\r' >&3
for _ in $(seq 1 50); do kill -0 "$qemu_pid" 2>/dev/null || break; sleep 0.2; done
stop_qemu
grep -q "fsck.*not cleanly unmounted; checked .*: clean" "$SCRATCH/clean.tmp"
check "the next boot noticed, checked, and found nothing wrong" $?
test "$(flags)" = clean
check "and shutdown left it clean" $?
host_fsck
check "  which the host's e2fsck agrees with" $?

rm -f "$SCRATCH"/{a,b,c,d,keep}.tmp "$SCRATCH/keep.out" \
      "$SCRATCH/hostfsck.tmp"

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
