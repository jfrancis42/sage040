#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fattest.sh - the FAT16 driver, which is no longer the machine's own
# filesystem and would otherwise be untested code in the kernel.
#
# The disk is ext2 now, and `mount_root()` probes ext2 first. fs/fat16.c
# is still built and still registered because a disk from a machine that
# has never heard of this one is a FAT disk -- so what this proves is
# that the fallback still works: a FAT volume mounts, is read, is
# written, and fsck.fat is happy with what the kernel left on it.
#
# It boots the kernel straight from its ELF rather than through the boot
# ROM, because the ROM reads ext2 only: a FAT disk has no KERNEL.ROM the
# ROM could find. That is the intended arrangement, not a limitation
# being worked around -- see bootrom/README.md.
#
# Runs on a scratch image. Requires mtools and dosfstools.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-fat.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/fattest.log"
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

if ! command -v mcopy >/dev/null || ! command -v mkfs.fat >/dev/null; then
    echo "fattest: mtools and dosfstools are needed for this one"
    echo "RESULT: FAIL"
    exit 1
fi

echo "=== building ==="
make -s kernel.elf || exit 1
make -s -C ../system sh || exit 1

echo "=== preparing a FAT16 $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n DOSVOL --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null

printf 'written by the host on a DOS disk\n' > "$SCRATCH/fathost.tmp"
mcopy -o -i "$MIMG" "$SCRATCH/fathost.tmp" ::/HOST.TXT
mmd -i "$MIMG" ::/SUB 2>/dev/null || true
mcopy -o -i "$MIMG" "$SCRATCH/fathost.tmp" ::/SUB/DEEP.TXT
LC_ALL=C.UTF-8 mcopy -o -i "$MIMG" "$SCRATCH/fathost.tmp" "::/A Long Name.txt"

echo "=== running the kernel against it ==="
printf '%s\n' \
  'cat HOST.TXT' \
  'cat /SUB/DEEP.TXT' \
  'cat "A Long Name.txt"' \
  'echo written by the guest > GUEST.TXT' \
  'cat GUEST.TXT' \
  'mkdir MADE' \
  'echo deeper > /MADE/INSIDE.TXT' \
  'rm HOST.TXT' \
  'ls' \
  'df' \
  'sync' \
  'halt' \
  > "$SCRATCH/fatsession.tmp"

rm -f "$SCRATCH/fatin.fifo"
mkfifo "$SCRATCH/fatin.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel kernel.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/fatin.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/fatin.fifo"
sleep "$BOOT_WAIT"
cat "$SCRATCH/fatsession.tmp" >&3

for _ in $(seq 1 300); do
    grep -qF "halting." "$LOG" 2>/dev/null && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.2
done
sleep 0.5
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/fatin.fifo"

sed 's/^/  | /' "$LOG"

echo "=== checks ==="
tr -d '\r' < "$LOG" > "$SCRATCH/fatclean.tmp"

grep -q "kernel ready." "$SCRATCH/fatclean.tmp"
check "the kernel booted" $?

# The whole point: ext2 was tried first and did not recognise it.
grep -q "fat16 on /dev/hda 'DOSVOL'" "$SCRATCH/fatclean.tmp"
check "the mount probe fell back to FAT16 and named the volume" $?

grep -qx "written by the host on a DOS disk" "$SCRATCH/fatclean.tmp"
check "read a file the host wrote" $?

grep -c "written by the host on a DOS disk" "$SCRATCH/fatclean.tmp" |
    grep -qE '^[3-9]|^[0-9]{2}'
check "  including one in a subdirectory and one with a long name" $?

grep -qx "written by the guest" "$SCRATCH/fatclean.tmp"
check "wrote a file and read it back" $?

echo "=== checks: what mtools sees afterwards ==="
mtype -i "$MIMG" ::/GUEST.TXT 2>/dev/null | tr -d '\r' |
    grep -qx "written by the guest"
check "the host reads the file the guest wrote" $?

mtype -i "$MIMG" ::/MADE/INSIDE.TXT 2>/dev/null | tr -d '\r' |
    grep -qx "deeper"
check "  and the one in the directory the guest made" $?

mdir -i "$MIMG" ::/ 2>&1 | grep -q "HOST"
check "the file the guest deleted is gone" $((1 - $?))

dd if="$DISK" of="$SCRATCH/fatpart.tmp" bs=512 skip="$PART_LBA" status=none
fsck.fat -n "$SCRATCH/fatpart.tmp" > "$SCRATCH/fatfsck.tmp" 2>&1
check "fsck.fat reports the volume clean" $?
sed 's/^/  | /' "$SCRATCH/fatfsck.tmp"

# A negative control: fsck.fat must be able to fail, or the check above
# says nothing.
python3 ./fatdamage.py "$DISK" "$OFFSET" lost >/dev/null 2>&1
dd if="$DISK" of="$SCRATCH/fatpart.tmp" bs=512 skip="$PART_LBA" status=none
! fsck.fat -n "$SCRATCH/fatpart.tmp" >/dev/null 2>&1
check "  and says so when the volume is damaged" $?

rm -f "$SCRATCH/fathost.tmp" "$SCRATCH/fatsession.tmp" \
      "$SCRATCH/fatclean.tmp" "$SCRATCH/fatpart.tmp" "$SCRATCH/fatfsck.tmp"

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ "$fail" -eq 0 ] && { echo "RESULT: PASS"; exit 0; }
echo "RESULT: FAIL"
exit 1
