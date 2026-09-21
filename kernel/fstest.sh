#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fstest.sh - drive the kernel's filesystem from the console, then check
# the result with the host's own MS-DOS tools.
#
# The point of the second half is the whole reason the disk is a real
# FAT16 volume: a filesystem the kernel alone can read proves nothing.
# What is checked here is that a file the kernel wrote comes back byte
# for byte through mtools, that a file the host wrote is what the kernel
# printed, and that fsck.fat finds nothing to complain about afterwards.
#
# Runs on a scratch image, so the machine's own disk is left alone.

set -u

cd "$(dirname "$0")"

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

DISK=hd-test.img
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG=fstest.log
BOOT_WAIT=${BOOT_WAIT:-4}

pass=0
fail=0

check() {           # check <description> <condition-exit-status>
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $1"
        fail=$((fail + 1))
    fi
}

contains() {        # contains <file> <text>
    grep -qF -- "$2" "$1"
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null

mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM

# A file written by the host, for the kernel to read back.
printf 'written by the host\nsecond line\n' > hostfile.tmp
mcopy -o -i "$MIMG" hostfile.tmp ::/HOST.TXT

# Something larger than one 2 KB cluster, to make the chain walk matter.
: > big.tmp
for i in $(seq 1 200); do
    printf 'line %03d 0123456789abcdefghijklmnopqrstuvwxyz\n' "$i" >> big.tmp
done
mcopy -o -i "$MIMG" big.tmp ::/BIG.TXT

echo "=== running the kernel ==="
printf '%s\n' \
  'uname -a' \
  'ls -l' \
  'cat HOST.TXT' \
  'cat > GUEST.TXT' \
  'a line the kernel wrote' \
  'and another one' \
  > session.tmp
printf '\004' >> session.tmp            # ctrl-D ends the input
printf '%s\n' \
  'cat GUEST.TXT' \
  'stat GUEST.TXT' \
  'cp BIG.TXT COPY.TXT' \
  'mv COPY.TXT RENAMED.TXT' \
  'ls -l' \
  'rm HOST.TXT' \
  'ls' \
  'df' \
  'cat NOSUCH.TXT' \
  'echo replaced > GUEST.TXT' \
  'cat GUEST.TXT' \
  'cat /dev/../nope' \
  'date -s 2001-02-03 04:05:06' \
  'date' \
  'sync' \
  'halt' \
  >> session.tmp

#
# The guest's "halt" stops the CPU, not the emulator, so the harness has
# to notice and kill QEMU -- the same thing ../tests/runtest.sh does.
# Feeding the session through a FIFO rather than a pipe keeps the write
# end open, so the kernel does not see an EOF partway through.
#
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

for _ in $(seq 1 300); do
    if grep -qF "halting." "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        break
    fi
    sleep 0.2
done

sleep 0.5
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f in.fifo

echo "=== guest session ==="
sed 's/^/  | /' "$LOG"

echo "=== checks: what the guest did ==="
contains "$LOG" "kernel ready."
check "kernel reached its shell" $?

contains "$LOG" "fat16 on /dev/hda 'SAGE040'"
check "mounted the host-created filesystem" $?

contains "$LOG" "written by the host"
check "read back a file the host wrote" $?

contains "$LOG" "no such file"
check "a missing file is reported, not a crash" $?

contains "$LOG" "Saturday, 3 February 2001"
check "the clock was set, and the weekday derived from the date" $?

contains "$LOG" "Sage040 0."
check "uname reported the kernel version" $?

contains "$LOG" "no such device"
check "a path under /dev that is not a device is refused" $?

contains "$LOG" "exception"
check "no exception was taken" $((1 - $?))

contains "$LOG" "RENAMED.TXT"
check "rename showed up in the directory" $?

echo "=== checks: what the host sees afterwards ==="
mdir -i "$MIMG" ::/ > dir.tmp 2>&1
grep -q "GUEST    TXT" dir.tmp
check "host sees GUEST.TXT" $?

grep -q "RENAMED  TXT" dir.tmp
check "host sees RENAMED.TXT" $?

grep -q "HOST     TXT" dir.tmp
check "host does not see the deleted HOST.TXT" $((1 - $?))

mtype -i "$MIMG" ::/GUEST.TXT > guest.tmp 2>/dev/null
[ "$(cat guest.tmp)" = "replaced" ]
check "GUEST.TXT holds exactly what the kernel last wrote" $?

# The kernel writes bare newlines, not CRLF: it is a Unix-flavoured
# system that happens to store files on an MS-DOS volume.
[ "$(wc -c < guest.tmp)" -eq 9 ]
check "the kernel wrote LF line endings, not CRLF" $?

mtype -i "$MIMG" ::/RENAMED.TXT > renamed.tmp 2>/dev/null
cmp -s renamed.tmp big.tmp
check "the kernel's copy is byte-identical to the original" $?

# fsck.fat has no idea what mtools' @@offset means, so hand it the
# partition on its own.
dd if="$DISK" of=part.tmp bs=512 skip="$PART_LBA" status=none
fsck.fat -n part.tmp > fsck.tmp 2>&1
check "fsck.fat reports the filesystem clean" $?
sed 's/^/  | /' fsck.tmp

echo
echo "  passed: $pass"
echo "  failed: $fail"

rm -f hostfile.tmp big.tmp session.tmp dir.tmp guest.tmp renamed.tmp \
      fsck.tmp part.tmp

[ "$fail" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$fail"
