#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fscktest.sh - checking and repairing the disk, and knowing when to.
#
# A FAT16 image is damaged on the host, in each of the ways fatdamage.py
# knows -- lost clusters, a cross-link, a broken chain, a size that does
# not fit, an orphaned long name, a wrong "..", disagreeing FAT copies --
# and the host's own fsck.fat is asked first, to prove the damage is
# real. Then the guest repairs it, and fsck.fat is asked again.
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

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-fsck.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/fscktest.log"
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
    printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
        | sfdisk -q "$DISK" >/dev/null
    mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
        $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
    mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
    mmd -i "$MIMG" ::/BIN ::/SUB
    mcopy -o -i "$MIMG" ../system/fsck ::/BIN/FSCK
    mcopy -o -i "$MIMG" ../system/shutdown ::/BIN/SHUTDOWN
    head -c 40000 /dev/urandom > "$SCRATCH/a.tmp"
    head -c 20000 /dev/urandom > "$SCRATCH/b.tmp"
    head -c 20000 /dev/urandom > "$SCRATCH/c.tmp"
    head -c 30000 /dev/urandom > "$SCRATCH/keep.tmp"
    echo small > "$SCRATCH/d.tmp"
    for f in a b c d keep; do
        mcopy -o -i "$MIMG" "$SCRATCH/$f.tmp" "::/$(echo $f | tr a-z A-Z).TXT"
    done
    LC_ALL=C.UTF-8 mcopy -o -i "$MIMG" "$SCRATCH/d.tmp" "::/Long Name For Orphan.txt"
    mcopy -o -i "$MIMG" "$SCRATCH/d.tmp" ::/SUB/X.TXT
}

damage() {
    local d=./fatdamage.py
    python3 $d "$DISK" "$OFFSET" lost
    python3 $d "$DISK" "$OFFSET" cross B.TXT A.TXT
    python3 $d "$DISK" "$OFFSET" broken C.TXT
    python3 $d "$DISK" "$OFFSET" size D.TXT
    python3 $d "$DISK" "$OFFSET" orphan LONGNA
    python3 $d "$DISK" "$OFFSET" dotdot SUB
    python3 $d "$DISK" "$OFFSET" fat2
}

host_fsck() {
    dd if="$DISK" of="$SCRATCH/part.tmp" bs=512 skip="$PART_LBA" status=none
    fsck.fat -n "$SCRATCH/part.tmp" > "$SCRATCH/hostfsck.tmp" 2>&1
    local rc=$?
    sed 's/^/  | host: /' "$SCRATCH/hostfsck.tmp"
    return $rc
}

# The flags, read off the disk: prints "dirty" or "clean" for each.
flags() {
    python3 - "$DISK" "$OFFSET" <<'PY'
import struct, sys
f = open(sys.argv[1], "rb"); base = int(sys.argv[2])
f.seek(base); b = f.read(512)
res = struct.unpack_from("<H", b, 14)[0]
f.seek(base + res * 512 + 2); fat1 = struct.unpack("<H", f.read(2))[0]
print("boot=" + ("dirty" if b[0x25] & 1 else "clean"),
      "fat1=" + ("clean" if fat1 & 0x8000 else "dirty"))
PY
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
python3 ./fatdamage.py "$DISK" "$OFFSET" dirty
host_fsck
test $? -ne 0
check "the host's fsck.fat finds the damage before the guest sees it" $?
grep -q "Orphan" "$SCRATCH/hostfsck.tmp" &&
    grep -q "share clusters" "$SCRATCH/hostfsck.tmp"
check "  including the orphaned long name and the cross-link" $?

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
check "the host's fsck.fat agrees it is clean" $?
test "$(flags)" = "boot=clean fat1=clean"
check "halt left both clean-unmount flags set" $?
LC_ALL=C mtype -i "$MIMG" ::/KEEP.TXT > "$SCRATCH/keep.out" 2>/dev/null
cmp -s "$SCRATCH/keep.tmp" "$SCRATCH/keep.out"
check "a file nothing damaged is byte for byte what it was" $?
LC_ALL=C mtype -i "$MIMG" ::/SUB/X.TXT 2>/dev/null | grep -qx small
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
for what in "FAT sectors where the two copies disagree" \
            "chains with a bad link" \
            "chains crossing another, or themselves" \
            "sizes that did not fit their chain" \
            "wrong . or .. entries" \
            "long-name entries with no file" \
            "lost clusters"; do
    grep -q "^  [0-9]* $what" "$SCRATCH/clean.tmp"
    check "fsck reports: $what" $?
done
test "$(grep -o 'FSCK-RC=[0-9][0-9]*' "$SCRATCH/clean.tmp" | tr '\n' ' ')" = \
     "FSCK-RC=4 FSCK-RC=1 FSCK-RC=0 "
check "exit status 4 unrepaired, 1 after -y, 0 when clean" $?
host_fsck
check "the host's fsck.fat agrees fsck -y repaired it" $?

echo "=== session 3: the flag itself ==="
make_image
start_qemu
printf 'echo written > /NOTE.TXT\r' >&3
sleep 1
kill "$qemu_pid" 2>/dev/null      # no halt: as a crash or a reset would
stop_qemu >/dev/null
test "$(flags)" = "boot=dirty fat1=clean"
check "a machine stopped without unmounting leaves the volume dirty" $?

# ...in the boot sector only. FAT[1]'s bit is never cleared, because
# mtools refuses a FAT whose second entry is not an end-of-chain value.
mdir -i "$MIMG" ::/ >/dev/null 2>&1
check "  and the host's mtools can still read it" $?

start_qemu
printf '/bin/shutdown\r' >&3
for _ in $(seq 1 50); do kill -0 "$qemu_pid" 2>/dev/null || break; sleep 0.2; done
stop_qemu
grep -q "fsck.*not cleanly unmounted; checked .*: clean" "$SCRATCH/clean.tmp"
check "the next boot noticed, checked, and found nothing wrong" $?
test "$(flags)" = "boot=clean fat1=clean"
check "and shutdown left it clean" $?
host_fsck
check "  which the host's fsck.fat agrees with" $?

rm -f "$SCRATCH"/{a,b,c,d,keep}.tmp "$SCRATCH/keep.out" "$SCRATCH/part.tmp" \
      "$SCRATCH/hostfsck.tmp"

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
