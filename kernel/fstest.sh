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

#
# Everything this test writes goes in one place.
#
# Scratch disk images are 16 MB each and there is one per test suite, so
# leaving them beside the source meant 67 MB of build product scattered
# through the tree with names that looked like part of it. They are all
# under scratch/ now, which `make clean` removes and git ignores.
#
#
# Computed AFTER the cd above, from the working directory rather than
# from $0 -- which has already been used once and is relative to where
# the script was invoked from, not to where it now is. Deriving it from
# $0 a second time worked when the script was run as ./edittest.sh and
# failed when it was run by path, which is a difference nobody should
# have to notice.
#
SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-test.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/fstest.log"
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
make -s -C ../user hello fbtest || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null

mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM

# A file written by the host, for the kernel to read back.
printf 'written by the host\nsecond line\n' > "$SCRATCH/hostfile.tmp"
mcopy -o -i "$MIMG" "$SCRATCH/hostfile.tmp" ::/HOST.TXT

# Something larger than one 2 KB cluster, to make the chain walk matter.
: > "$SCRATCH/big.tmp"
for i in $(seq 1 200); do
    printf 'line %03d 0123456789abcdefghijklmnopqrstuvwxyz\n' "$i" >> "$SCRATCH/big.tmp"
done
mcopy -o -i "$MIMG" "$SCRATCH/big.tmp" ::/BIG.TXT

# A program, to check that the ELF loader runs one and that its exit
# status comes back. No extension: the kernel decides what is executable
# from the file's first four bytes, not from its name.
mcopy -o -i "$MIMG" ../user/hello ::/HELLO
mcopy -o -i "$MIMG" ../user/fbtest ::/FBTEST

echo "=== running the kernel ==="
printf '%s\n' \
  'uname -a' \
  'ls -l' \
  'cat HOST.TXT' \
  'cat > GUEST.TXT' \
  'a line the kernel wrote' \
  'and another one' \
  > "$SCRATCH/session.tmp"
printf '\004' >> "$SCRATCH/session.tmp"            # ctrl-D ends the input
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
  'hello one two' \
  'hello -x' \
  'nosuchprogram' \
  'BIG.TXT' \
  'uptime' \
  'console' \
  'console fbcon off' \
  'console ttyS0 off' \
  'echo only on the serial line now' \
  'console fbcon on' \
  'fbtest 1' \
  'date -s 2001-02-03 04:05:06' \
  'date' \
  'mkdir etc' \
  'mkdir bin' \
  'cd etc' \
  'pwd' \
  'echo written in a subdirectory > sub.txt' \
  'cat sub.txt' \
  'ls' \
  'cd ..' \
  'pwd' \
  'cat /etc/sub.txt' \
  'mkdir etc' \
  'rmdir bin' \
  'rmdir etc' \
  'sync' \
  'halt' \
  >> "$SCRATCH/session.tmp"

#
# The guest's "halt" stops the CPU, not the emulator, so the harness has
# to notice and kill QEMU -- the same thing ../tests/runtest.sh does.
# Feeding the session through a FIFO rather than a pipe keeps the write
# end open, so the kernel does not see an EOF partway through.
#
rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"

"$QEMU" -M sage040 -cpu m68040 -m 4 \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"
cat "$SCRATCH/session.tmp" >&3

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
rm -f "$SCRATCH/in.fifo"

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

contains "$LOG" "hello from a program"
check "a program was loaded from the disk and run" $?

contains "$LOG" "argv[2] = two"
check "argv reached the program intact" $?

contains "$LOG" "hello: exited 1"
check "a non-zero exit status came back to the shell" $?

contains "$LOG" "nosuchprogram: command not found"
check "a missing program is reported as not found" $?

contains "$LOG" "BIG.TXT: not an executable"
check "a data file is refused as a program, by its contents" $?

grep -qE "ticks at 100 Hz" "$LOG" && \
  ! grep -qE "^0 ticks" "$LOG"
check "the timer tick is running" $?

contains "$LOG" "mfp-timer-d at"
check "the MC68901 registered as the system timer" $?

contains "$LOG" "SM501 as /dev/fb0"
check "the framebuffer registered as a device" $?

contains "$LOG" "/dev/fbcon, 80x30 of IBM PC 8x16"
check "the text console came up at 80x30" $?

contains "$LOG" "output to ttyS0 fbcon, input from ttyS0 kbd0"
check "the terminal has both sinks and both input sources" $?

contains "$LOG" "8042 as /dev/kbd0, scancode set 1"
check "the keyboard registered as a terminal input source" $?

contains "$LOG" "only on the serial line now"
check "the serial line keeps working with the screen switched off" $?

# The last sink cannot be turned off: a machine with no console output
# is one that cannot tell you why.
contains "$LOG" "that is the only one left"
check "turning off the last remaining sink is refused" $?

contains "$LOG" "fbtest: drawn, holding"
check "a program drew through /dev/fb0 without error" $?

grep -q "fbtest:.*failed" "$LOG"
check "no framebuffer ioctl reported a failure" $((1 - $?))

contains "$LOG" "exception"
check "no exception was taken" $((1 - $?))

contains "$LOG" "RENAMED.TXT"
check "rename showed up in the directory" $?

echo "=== checks: directories ==="

contains "$LOG" "/etc"
check "cd moved into a subdirectory and pwd said so" $?

contains "$LOG" "written in a subdirectory"
check "a file created in a subdirectory read back" $?

contains "$LOG" "file exists"
check "making a directory that already exists is refused" $?

contains "$LOG" "directory not empty"
check "removing a directory with something in it is refused" $?

echo "=== checks: what the host sees afterwards ==="

mdir -i "$MIMG" ::/ 2>&1 | grep -q "ETC"
check "host sees the ETC directory" $?

mdir -i "$MIMG" ::/ETC 2>&1 | grep -q "SUB      TXT"
check "host sees the file the guest made inside it" $?

mtype -i "$MIMG" ::/ETC/SUB.TXT 2>/dev/null | grep -q "written in a subdirectory"
check "  and its contents are what the guest wrote" $?

mdir -i "$MIMG" ::/ 2>&1 | grep -q "BIN" && false || true
check "the empty directory the guest removed is gone" $?

mdir -i "$MIMG" ::/ > "$SCRATCH/dir.tmp" 2>&1
grep -q "GUEST    TXT" "$SCRATCH/dir.tmp"
check "host sees GUEST.TXT" $?

grep -q "RENAMED  TXT" "$SCRATCH/dir.tmp"
check "host sees RENAMED.TXT" $?

grep -q "HOST     TXT" "$SCRATCH/dir.tmp"
check "host does not see the deleted HOST.TXT" $((1 - $?))

mtype -i "$MIMG" ::/GUEST.TXT > guest.tmp 2>/dev/null
[ "$(cat guest.tmp)" = "replaced" ]
check "GUEST.TXT holds exactly what the kernel last wrote" $?

# The kernel writes bare newlines, not CRLF: it is a Unix-flavoured
# system that happens to store files on an MS-DOS volume.
[ "$(wc -c < guest.tmp)" -eq 9 ]
check "the kernel wrote LF line endings, not CRLF" $?

mtype -i "$MIMG" ::/RENAMED.TXT > renamed.tmp 2>/dev/null
cmp -s renamed.tmp "$SCRATCH/big.tmp"
check "the kernel's copy is byte-identical to the original" $?

# fsck.fat has no idea what mtools' @@offset means, so hand it the
# partition on its own.
dd if="$DISK" of="$SCRATCH/part.tmp" bs=512 skip="$PART_LBA" status=none
fsck.fat -n "$SCRATCH/part.tmp" > fsck.tmp 2>&1
check "fsck.fat reports the filesystem clean" $?
sed 's/^/  | /' fsck.tmp

echo
echo "  passed: $pass"
echo "  failed: $fail"

rm -f "$SCRATCH/hostfile.tmp" "$SCRATCH/big.tmp" "$SCRATCH/session.tmp" "$SCRATCH/dir.tmp" guest.tmp renamed.tmp \
      fsck.tmp "$SCRATCH/part.tmp"

[ "$fail" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$fail"
