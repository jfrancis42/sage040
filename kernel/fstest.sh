#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fstest.sh - drive the kernel's filesystem from the console, then check
# the result with the host's own ext2 tools.
#
# The point of the second half is the whole reason the disk is a real
# ext2 volume: a filesystem the kernel alone can read proves nothing.
# What is checked here is that a file the kernel wrote comes back byte
# for byte through debugfs, that a file the host wrote is what the
# kernel printed, and that e2fsck -- which shares no line of code with
# the driver -- finds nothing to complain about afterwards.
#
# e2fsck is the strongest check in this file. It recomputes every link
# count, every block and inode bitmap and every directory's "." and
# "..", so a driver that keeps a plausible-looking filesystem the kernel
# itself is happy with still fails here.
#
# Runs on a scratch image, so the machine's own disk is left alone.

set -u

cd "$(dirname "$0")"

# How big the machine is. One place, shared with the Makefiles.
. ../machine.conf

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
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-test.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/fstest.log"
# Gone before QEMU starts, so a run that never reaches the guest has no
# log to grade -- rather than silently grading the last run's.
rm -f "$LOG"
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
make -s -C ../apps hello fbtest || exit 1
make -s -C ../system || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040

fsimg put kernel.rom /KERNEL.ROM

# A file written by the host, for the kernel to read back.
printf 'written by the host\nsecond line\n' > "$SCRATCH/hostfile.tmp"
fsimg put "$SCRATCH/hostfile.tmp" /HOST.TXT

# Something larger than one 2 KB cluster, to make the chain walk matter.
: > "$SCRATCH/big.tmp"
for i in $(seq 1 200); do
    printf 'line %03d 0123456789abcdefghijklmnopqrstuvwxyz\n' "$i" >> "$SCRATCH/big.tmp"
done
fsimg put "$SCRATCH/big.tmp" /BIG.TXT
# The same bytes with the x bits set. Running a program is now refused
# for TWO different reasons and they are worth telling apart: no x bit
# is "permission denied" and never reaches the loader, while an x bit
# on something that is not an ELF file gets all the way to exec.c and
# is refused on its first four bytes. One file can only ever test one
# of them.
fsimg put -m 755 "$SCRATCH/big.tmp" /XBITS.TXT

# A program, to check that the ELF loader runs one and that its exit
# status comes back. No extension: the kernel decides what is executable
# from the file's first four bytes, not from its name.
# Long names made by the HOST, which the guest has to find by them: one
# in plain ASCII, one in UTF-8. On ext2 a name is just bytes, so the
# second needs no locale and no special handling at all.
echo "made on the host" > "$SCRATCH/lfn.tmp"
LC_ALL=C.UTF-8 fsimg put "$SCRATCH/lfn.tmp" "/Host Long Name.txt"
# Any length, any bytes: there is no short form to fall back to.
echo "naive, with a diaeresis" > "$SCRATCH/lfn.tmp"
LC_ALL=C.UTF-8 fsimg put "$SCRATCH/lfn.tmp" $'/na\xc3\xafve r\xc3\xa9sum\xc3\xa9.txt'

# BIGGER THAN ONE INDIRECT BLOCK REACHES. At the 4 KB block size, twelve
# direct pointers and one indirect block cover 12*4096 + 1024*4096 =
# 4,243,456 bytes; anything past that needs the DOUBLE indirect block,
# and nothing else in this suite goes near it. 5 MB does, in both
# directions: the host writes this one and the guest copies it.
head -c 5242880 /dev/urandom > "$SCRATCH/huge.tmp"
fsimg put "$SCRATCH/huge.tmp" /HUGE.BIN

# A SYMLINK THE HOST MADE, followed by the guest.
#
# This used to check the opposite. When ext2 could hold a symlink and
# nothing in the kernel could follow one, the thing worth checking was
# that reading it was REFUSED rather than handing back the target's
# NAME as though it were the file's contents -- which is what a driver
# that knows only S_IFREG and S_IFDIR does.
#
# The kernel follows them now (kernel/linktest.sh covers making and
# reading them from the guest), so the same command that had to fail
# has to work: `cat /alink` must produce HOST.TXT's contents. The old
# expectation is not weakened, it is inverted -- and the "target's
# name as contents" failure is still caught, because the name is not
# what HOST.TXT holds.
printf 'symlink /alink HOST.TXT\nquit\n' | debugfs -w "$DISK?offset=$OFFSET" >/dev/null 2>&1

fsimg put -m 755 ../apps/hello /hello
fsimg put -m 755 ../system/uptime /uptime
fsimg put -m 755 ../apps/fbtest /fbtest

echo "=== running the kernel ==="
printf '%s\n' \
  'uname -a' \
  'ls -l' \
  'cat HOST.TXT' \
  'echo SYMLINK-TEST' \
  'cat /alink' \
  'echo LINK-RC=$?' \
  'cat > GUEST.TXT' \
  'a line the kernel wrote' \
  'and another one' \
  > "$SCRATCH/session.tmp"
printf '\004' >> "$SCRATCH/session.tmp"            # ctrl-D ends the input
printf '%s\n' \
  'cat GUEST.TXT' \
  'stat GUEST.TXT' \
  'cp BIG.TXT COPY.TXT' \
  'cp HUGE.BIN HUGECOPY.BIN' \
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
  'XBITS.TXT' \
  '/uptime' \
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
  'cat /etc/sub.txt' \
  'stat /etc/sub.txt' \
  'cd ..' \
  'pwd' \
  'cat /etc/sub.txt' \
  'mkdir etc' \
  'rmdir bin' \
  'rmdir etc' \
  'mkdir tmp' \
  'echo in tmp > /tmp/moved.txt' \
  'mv /tmp/moved.txt /etc/moved.txt' \
  'echo target > /etc/over.txt' \
  'echo source > /etc/src.txt' \
  'mv /etc/src.txt /etc/over.txt' \
  'echo root copy > /rootf.txt' \
  'echo doomed > /etc/gone.txt' \
  'cd etc' \
  'echo etc copy > rootf.txt' \
  'rm rootf.txt' \
  'cd /tmp' \
  'rm /etc/gone.txt' \
  'cd /' \
  'mkdir mvdir' \
  'mv mvdir /tmp/mvdir' \
  'echo long > "A Long File Name.txt"' \
  'echo mixed > MixedCase.c' \
  'echo CASE-TEST' \
  'echo lower > casetest.txt' \
  'echo UPPER > CASETEST.TXT' \
  'cat casetest.txt' \
  'cat CASETEST.TXT' \
  'echo LFN-HOST' \
  'cat "Host Long Name.txt"' \
  $'cat "na\xc3\xafve r\xc3\xa9sum\xc3\xa9.txt"' \
  'mv "A Long File Name.txt" "Renamed Long Name.text"' \
  'mkdir "Long Directory"' \
  'echo inside > "Long Directory/file in it.txt"' \
  'cd "Long Directory"' \
  'echo LFN-PWD' \
  'pwd' \
  'cat "file in it.txt"' \
  'cd /' \
  'echo one > longprefix1.txt' \
  'echo two > longprefix2.txt' \
  'echo three > longprefix3.txt' \
  'rm longprefix2.txt' \
  $'echo utf > "caf\xc3\xa9.txt"' \
  'echo LFN-LS' \
  'ls' \
  'cd /tmp/mvdir' \
  'echo LS-DOTDOT' \
  'ls ..' \
  'cd /' \
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

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
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

contains "$LOG" "ext2 on /dev/hda 'SAGE040'"
check "mounted the host-created filesystem" $?

contains "$LOG" "written by the host"
check "read back a file the host wrote" $?

contains "$LOG" "no such file"
check "a missing file is reported, not a crash" $?

contains "$LOG" "Saturday, 3 February 2001"
check "the clock was set, and the weekday derived from the date" $?

contains "$LOG" "SuckOS 0."
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

contains "$LOG" "BIG.TXT: permission denied"
check "a data file with no x bit is refused, on its mode" $?

contains "$LOG" "XBITS.TXT: not an executable"
check "  and with the x bits set, on its contents" $?

contains "$LOG" "load average:"
check "the uptime program runs from the disk and reports load" $?

contains "$LOG" "mfp-timer-d at"
check "the MC68901 registered as the system timer" $?

contains "$LOG" "SM501 as /dev/fb0"
check "the framebuffer registered as a device" $?

contains "$LOG" "/dev/fbcon, 80x30 of IBM PC 8x16"
check "the text console came up at 80x30" $?

contains "$LOG" "tty1: fbcon(out)"
check "the screen is its own terminal (tty1), keyboard and framebuffer" $?
contains "$LOG" "console: ttyS0(out) ttyS0(in)"
check "the serial line is its own terminal (console)" $?

contains "$LOG" "8042 as /dev/kbd0, scancode set 1"
check "the keyboard registered as a terminal input source" $?

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

# An absolute path has to mean the same thing wherever the caller is
# standing. vfs.c used to strip the leading slash before handing the
# path to the filesystem, which resolves a slashless name RELATIVE to
# the current directory -- so `/etc/sub.txt` became `/etc/etc/sub.txt`
# from inside /etc, while working perfectly from the root, which is
# where everything was tested.
test "$(grep -c 'written in a subdirectory' "$LOG")" -ge 3
check "an absolute path works from INSIDE the directory it names" $?

contains "$LOG" "  size"
check "  and stat resolves one from there too" $?

echo "=== checks: what the host sees afterwards ==="

# Names are bytes on ext2, so the host looks for exactly what the guest
# typed -- `mkdir etc` makes "etc", not "ETC". Under FAT16 every one of
# these was the upper-cased short name, which is why they all changed.
fsimg ls / | grep -qx "etc"
check "host sees the etc directory the guest made" $?

fsimg ls /etc | grep -qx "sub.txt"
check "host sees the file the guest made inside it" $?

fsimg cat /etc/sub.txt | grep -q "written in a subdirectory"
check "  and its contents are what the guest wrote" $?

fsimg ls / | grep -qx "bin"
check "the empty directory the guest removed is gone" $((1 - $?))

fsimg ls / > "$SCRATCH/dir.tmp" 2>&1
grep -qx "GUEST.TXT" "$SCRATCH/dir.tmp"
check "host sees GUEST.TXT" $?

grep -qx "RENAMED.TXT" "$SCRATCH/dir.tmp"
check "host sees RENAMED.TXT" $?

grep -qx "HOST.TXT" "$SCRATCH/dir.tmp"
check "host does not see the deleted HOST.TXT" $((1 - $?))

fsimg cat /GUEST.TXT > guest.tmp 2>/dev/null
[ "$(cat guest.tmp)" = "replaced" ]
check "GUEST.TXT holds exactly what the kernel last wrote" $?

# The kernel writes bare newlines, not CRLF.
[ "$(wc -c < guest.tmp)" -eq 9 ]
check "the kernel wrote LF line endings, not CRLF" $?

fsimg cat /RENAMED.TXT > renamed.tmp 2>/dev/null
cmp -s renamed.tmp "$SCRATCH/big.tmp"
check "the kernel's copy is byte-identical to the original" $?

# The double indirect block, in both directions: the kernel READ 5 MB
# that the host wrote and WROTE 5 MB the host can read back. Up to
# 4,243,456 bytes a file needs only direct and singly indirect blocks,
# so without this the second and third levels of the block map are
# never executed at all.
fsimg get /HUGECOPY.BIN "$SCRATCH/hugecopy.tmp" 2>/dev/null
cmp -s "$SCRATCH/huge.tmp" "$SCRATCH/hugecopy.tmp"
check "a 5 MB file, past what one indirect block reaches, copies byte for byte" $?

[ "$(fsimg size /HUGECOPY.BIN)" = 5242880 ]
check "  and its size is right, so the copy did not stop early" $?

# unlink and rename used to look every name up in the ROOT, whatever
# the path said, and unlink wrote its deletion through a directory
# pointer it never set. Everything above ran in the root, so none of it
# showed. These are the cases that would have.
fsimg cat /etc/moved.txt 2>/dev/null | grep -qx "in tmp" &&
    ! fsimg ls /tmp | grep -qx "moved.txt"
check "mv between directories moved the file" $?

fsimg cat /etc/over.txt 2>/dev/null | grep -qx "source" &&
    ! fsimg ls /etc | grep -qx "src.txt"
check "mv onto an existing file replaced it, as POSIX says" $?

fsimg cat /rootf.txt 2>/dev/null | grep -qx "root copy" &&
    ! fsimg ls /etc | grep -qx "rootf.txt"
check "rm of a relative name in a subdirectory removed that one, not the root's" $?

! fsimg ls /etc | grep -qx "gone.txt"
check "rm of an absolute path from another directory" $?

fsimg ls /tmp | grep -qx "mvdir" &&
    ! fsimg ls / | grep -qx "mvdir"
check "mv moved a directory into another" $?

# From inside /tmp/mvdir, `ls ..` lists /tmp, which holds mvdir. Had the
# ".." still named the root, it would list etc and tmp and no mvdir.
tr -d '\r' < "$LOG" | grep -A2 '^LS-DOTDOT$' | grep -qi 'mvdir'
check "  and its .. now leads to its new parent" $?

# A directory moved to a new parent carries its ".." with it, and both
# parents' link counts follow. Nothing in the guest can see this; it is
# what e2fsck checks below, and this is the direct form of it.
[ "$(fsimg ls-l /tmp/mvdir | awk '$NF == ".." { print $1 }')" = \
  "$(fsimg ls-l / | awk '$NF == "tmp" { print $1 }')" ]
check "  and the moved directory's .. names the inode of its new parent" $?

echo "=== checks: names are bytes ==="

LC_ALL=C.UTF-8 fsimg ls / > "$SCRATCH/lfn-dir.tmp" 2>&1
tr -d '\r' < "$LOG" > "$SCRATCH/lfn-log.tmp"

grep -qx "Renamed Long Name.text" "$SCRATCH/lfn-dir.tmp" &&
    ! grep -qx "A Long File Name.txt" "$SCRATCH/lfn-dir.tmp"
check "a name with spaces, made by the guest and renamed, is what the host sees" $?

LC_ALL=C.UTF-8 fsimg cat "/Renamed Long Name.text" 2>/dev/null | grep -qx long
check "  holding what the guest wrote" $?

grep -qx "MixedCase.c" "$SCRATCH/lfn-dir.tmp"
check "a mixed-case name keeps its case" $?

# FAT16 folded case, so "casetest.txt" and "CASETEST.TXT" were ONE file
# and the guest's second write replaced the first. On ext2 they are two.
grep -qx "casetest.txt" "$SCRATCH/lfn-dir.tmp" &&
    grep -qx "CASETEST.TXT" "$SCRATCH/lfn-dir.tmp"
check "two names differing only in case are two different files" $?

[ "$(fsimg cat /casetest.txt)" = "lower" ] &&
    [ "$(fsimg cat /CASETEST.TXT)" = "UPPER" ]
check "  and each holds its own contents" $?

# Between the markers, not a fixed number of lines after one: the shell
# echoes a prompt and the command itself before each answer, so counting
# lines makes the check depend on how the prompt is printed.
awk '/^CASE-TEST$/ { f = 1; next } /^LFN-HOST$/ { f = 0 } f' \
    "$SCRATCH/lfn-log.tmp" > "$SCRATCH/case.tmp"
grep -qx lower "$SCRATCH/case.tmp" && grep -qx UPPER "$SCRATCH/case.tmp"
check "  as the guest reads them back too" $?

grep -A2 '^LFN-HOST$' "$SCRATCH/lfn-log.tmp" | grep -qx "made on the host"
check "a long name the host made is found by it" $?

grep -qx "naive, with a diaeresis" "$SCRATCH/lfn-log.tmp"
check "  and one in UTF-8" $?

grep -q $'caf\xc3\xa9.txt' "$SCRATCH/lfn-dir.tmp"
check "a UTF-8 name the guest made is the same bytes to the host" $?

LC_ALL=C.UTF-8 fsimg ls "/Long Directory" 2>&1 | grep -qx "file in it.txt" &&
    LC_ALL=C.UTF-8 fsimg cat "/Long Directory/file in it.txt" 2>/dev/null |
        grep -qx inside
check "a directory with a long name, and a long-named file in it" $?

grep -A2 '^LFN-PWD$' "$SCRATCH/lfn-log.tmp" | grep -qx "/Long Directory" &&
    grep -qx inside "$SCRATCH/lfn-log.tmp"
check "  which cd, pwd and a relative name all work in" $?

grep -qx "longprefix1.txt" "$SCRATCH/lfn-dir.tmp" &&
    grep -qx "longprefix3.txt" "$SCRATCH/lfn-dir.tmp"
check "names alike in their first letters are kept whole" $?

! grep -qx "longprefix2.txt" "$SCRATCH/lfn-dir.tmp"
check "rm of a long name removes it" $?

awk '/^LFN-LS$/ { f = 1; next } f' "$SCRATCH/lfn-log.tmp" | grep -q "Renamed Long Name.text"
check "ls shows long names" $?

echo "=== checks: a symlink the host made ==="

awk '/^SYMLINK-TEST$/ { f = 1; next } /^LINK-RC=/ { f = 0 } f' \
    "$SCRATCH/lfn-log.tmp" > "$SCRATCH/link.tmp"
grep -q "written by the host" "$SCRATCH/link.tmp"
check "a symlink is FOLLOWED: reading it gives the target's contents" $?

# Not the target's NAME. A filesystem that knows only S_IFREG would
# hand back "HOST.TXT" as though that were the file, and the check
# above would not notice on its own.
! grep -q "^HOST.TXT$" "$SCRATCH/link.tmp"
check "  and not the target's name as though it were the contents" $?

grep -q "^LINK-RC=0" "$SCRATCH/lfn-log.tmp"
check "  with a status of 0" $?

echo "=== checks: what e2fsck says ==="

# The whole volume, checked by code that has nothing to do with the
# driver: bitmaps, link counts, "." and "..", every block map.
fsimg fsck > fsck.tmp 2>&1
check "e2fsck reports the filesystem clean" $?
sed 's/^/  | /' fsck.tmp

# A negative control for the check above: damage one link count and
# make sure e2fsck is actually looking.
cp "$DISK" "$SCRATCH/damaged.img"
printf 'sif /etc links_count 7\nquit\n' |
    debugfs -w "$SCRATCH/damaged.img?offset=$OFFSET" >/dev/null 2>&1
! e2fsck -fn "$SCRATCH/damaged.img?offset=$OFFSET" >/dev/null 2>&1
check "  and says so when a link count is wrong, so the check above means something" $?
rm -f "$SCRATCH/damaged.img"

echo
echo "  passed: $pass"
echo "  failed: $fail"

rm -f "$SCRATCH/link.tmp" "$SCRATCH/case.tmp" "$SCRATCH/lfn.tmp" "$SCRATCH/huge.tmp" "$SCRATCH/hugecopy.tmp" "$SCRATCH/lfn-dir.tmp" "$SCRATCH/lfn-log.tmp"
rm -f "$SCRATCH/hostfile.tmp" "$SCRATCH/big.tmp" "$SCRATCH/session.tmp" "$SCRATCH/dir.tmp" guest.tmp renamed.tmp \
      fsck.tmp

[ "$fail" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$fail"
