#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# linktest.sh - hard links, and what they are not.
#
# A hard link is one inode with two directory entries. There is no
# original and no copy: the names are equal, writing through either is
# writing the same file, and the file goes when the LAST name does.
# Every check here is aimed at one of those three claims, and the
# INODE NUMBERS are compared rather than the contents -- two files
# that happen to hold the same bytes would pass a content check
# whatever the kernel did.
#
# The link count is read back with the HOST's tools afterwards, and the
# volume checked with e2fsck: the kernel saying "2" about its own work
# proves nothing, and a link count that disagrees with the number of
# names is exactly the corruption this could cause.

set -u
cd "$(dirname "$0")"
. ../machine.conf

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-link.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "linktest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/linktest.log"; rm -f "$LOG" "$SCRATCH/lk.fifo"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../ldso || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || true

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib
# THE DYNAMIC LOADER AND THE C LIBRARY. sbase's programs are linked
# against libc.so, so without these every one of them exits 126 with
# "cannot access a needed shared library" -- and a suite that did not
# notice would go on to test nothing at all. That is precisely what
# happened here: `ln` never ran, `echo > /two.txt` made an ordinary
# file, and six checks passed against a machine with no hard links in
# it. See the LN=0 guard below.
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../system/sh /bin/sh
# OPTIONAL ones go round the strict helper on purpose: `exit` inside a
# shell function exits the SCRIPT, so `fsimg ... || true` cannot catch
# it -- the suite died silently after "preparing" because sbase has no
# stat(1) and the helper took the whole thing down with it.
for p in ln ls cat echo rm mkdir cp readlink; do
    PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" put -m 755 \
        "../ports/sbase/bin/$p" /bin/$p >/dev/null 2>&1 ||
        echo "linktest: no sbase $p (skipped)" >&2
done
printf 'the original contents\n' > "$SCRATCH/lk.tmp"
fsimg put "$SCRATCH/lk.tmp" /one.txt
fsimg mkdir /adir

mkfifo "$SCRATCH/lk.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/lk.fifo" > "$LOG" 2>&1 &
pid=$!
exec 3> "$SCRATCH/lk.fifo"
sleep 5
send(){ printf '%s\r' "$1" >&3; sleep "${2:-1}"; }

send '/bin/ln /one.txt /two.txt; echo LN=$?'
send '/bin/ls -li /one.txt'
send '/bin/ls -li /two.txt'
send 'cat /two.txt'
# Writing through the second name must change the first: same inode.
send 'echo CHANGED > /two.txt'
send 'cat /one.txt'
# Removing one name leaves the other, with the contents.
send 'rm /one.txt; echo RM=$?'
send 'cat /two.txt'
# A directory may not be linked, and an existing name may not be
# clobbered. Both must be REFUSED, and each with its own message.
send '/bin/ln /adir /dirlink; echo DIRLN=$?'
send '/bin/ln /two.txt /two.txt; echo SAMELN=$?'
# --- SYMBOLIC LINKS ---------------------------------------------------
send '/bin/ln -s /two.txt /slink; echo SLN=$?'
send '/bin/echo via-symlink > /two.txt'
send '/bin/cat /slink'
# readlink gives the target back, unfollowed.
send '/bin/readlink /slink'
# ls -l must show it AS A LINK (l at the front, -> target), which means
# statx honoured AT_SYMLINK_NOFOLLOW.
send '/bin/ls -l /slink'
# A relative target is relative to the DIRECTORY THE LINK IS IN, not to
# the working directory -- the case that is wrong in most first
# attempts. /adir/rel -> one.txt must mean /adir/one.txt.
send '/bin/echo in-adir > /adir/one.txt'
send '/bin/ln -s one.txt /adir/rel; echo RELLN=$?'
send '/bin/cat /adir/rel'
# Through a link in an INTERIOR component.
send '/bin/ln -s /adir /dlink; echo DLN=$?'
send '/bin/cat /dlink/one.txt'
# A DANGLING link is legal to make and fails to open.
send '/bin/ln -s /nowhere /dangle; echo DANGLE=$?'
send '/bin/cat /dangle; echo CATDANGLE=$?'
send '/bin/readlink /dangle'
# A LOOP must be ELOOP, not a hung machine.
send '/bin/ln -s /loopb /loopa; echo LA=$?'
send '/bin/ln -s /loopa /loopb; echo LB=$?'
send '/bin/cat /loopa; echo LOOP=$?' 3
# A LONG target goes in a block rather than in the inode; both shapes
# have to read back.
send '/bin/ln -s /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/deep/target /longlink; echo LONGLN=$?'
send '/bin/readlink /longlink'
send 'echo ALL-DONE' 2
for i in $(seq 1 60); do grep -q 'ALL-DONE' "$LOG" && break; sleep 1; done
send 'halt' 2
sleep 2
exec 3>&-; kill $pid 2>/dev/null; wait $pid 2>/dev/null

CLEAN="$SCRATCH/linktest-clean.tmp"
tr -d '\r' < "$LOG" > "$CLEAN"
echo "=== guest session ==="
sed -n '/kernel ready/,$p' "$CLEAN" | sed 's/^/  | /'

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }
has(){ grep -qF "$1" "$CLEAN"; }

echo "=== checks: on the machine ==="
has 'LN=0'        ; check "ln made a second name" $?
# EVERYTHING BELOW DEPENDS ON THAT. If ln did not run, the rest of the
# session tests an ordinary file being written and removed, which
# passes and means nothing -- so say so once and stop, rather than
# print a column of OKs about a machine with no links in it.
if ! has 'LN=0'; then
    echo "  [FAIL] ln did not run; the remaining checks would be vacuous"
    fail=$((fail + 1))
    echo; echo "  passed: $pass"; echo "  failed: $fail"
    echo "RESULT: FAIL"; exit 1
fi
# The two names, same inode. `ls -li` prints the inode number first, so
# the field has to BE a number -- matching the prompt line and
# comparing "/$" with "/$" is how this passed while nothing worked.
a=$(grep -E '^[0-9]+ .*/one\.txt$' "$CLEAN" | head -1 | awk '{print $1}')
b=$(grep -E '^[0-9]+ .*/two\.txt$' "$CLEAN" | head -1 | awk '{print $1}')
case "${a:-x}" in ''|*[!0-9]*) a=; esac
[ -n "$a" ] && [ "$a" = "$b" ]
check "  and both names are the SAME inode (${a:-not a number})" $?
has 'the original contents'
check "  the new name reaches the contents" $?
# `ls -l`'s third field is the link count, and it must say 2 while both
# names exist. It said 1 until statx reported what ext2 recorded --
# the count on disk was right the whole time, so only a check that
# looked through userspace could see it was wrong.
n=$(grep -E '^[0-9]+ .*/one\.txt$' "$CLEAN" | head -1 | awk '{print $3}')
test "$n" = "2"
check "  and ls shows a link count of 2 (got ${n:-none})" $?
has 'CHANGED'     ; check "writing through one name changes the other" $?
has 'RM=0'        ; check "one name can be removed" $?
[ "$(grep -c 'CHANGED' "$CLEAN")" -ge 2 ]
check "  and the file survives, reachable by the name that is left" $?
has 'DIRLN=0' && r=1 || r=0
check "a hard link to a DIRECTORY is refused" $r
has 'SAMELN=0' && r=1 || r=0
check "  and so is a name that already exists" $r

echo "=== checks: symbolic links ==="
has 'SLN=0'   ; check "ln -s made a symbolic link" $?
# NOT 'CHANGED': that string is already in the log from the hard-link
# section above, so the check passed while `cat /slink` was in fact
# failing with ELOOP. A marker this section alone can produce.
[ "$(grep -c 'via-symlink' "$CLEAN")" -ge 2 ]
check "  and reading through it reaches the target" $?
grep -qx '/two.txt' "$CLEAN"
check "  readlink gives the target back, unfollowed" $?
grep -qE '^l.*slink -> /two\.txt' "$CLEAN"
check "  ls -l shows it AS a link (statx honoured AT_SYMLINK_NOFOLLOW)" $?
has 'RELLN=0' ; check "a relative target is made" $?
has 'in-adir'
check "  and resolves against the LINK's directory, not the cwd" $?
has 'DLN=0'   ; check "a link to a directory is allowed (unlike a hard link)" $?
[ "$(grep -c 'in-adir' "$CLEAN")" -ge 2 ]
check "  and a path THROUGH it resolves" $?
has 'DANGLE=0'
check "a dangling link can be made -- the target need not exist" $?
has 'CATDANGLE=0' && r=1 || r=0
check "  and opening it fails" $r
grep -qx '/nowhere' "$CLEAN"
check "  while readlink still reports its target" $?
has 'LOOP=0' && r=1 || r=0
check "a symlink loop is refused, not followed for ever" $r
has 'LONGLN=0'
check "a target too long for the inode is stored in a block" $?
grep -q '/deep/target' "$CLEAN"
check "  and reads back whole" $?

echo "=== checks: on the host ==="
# The link count is what the kernel wrote to the inode, read by
# somebody else. After one name was removed it must be 1 again.
lc=$(fsimg ls-l / 2>/dev/null | awk '$NF=="two.txt"{print $3}' | tr -d '()')
test "$lc" = "1"
check "the host reads a link count of 1 after the other name went (got ${lc:-none})" $?
# Round the strict helper: `exists` returning non-zero is the ANSWER
# here, not a failure, and the helper would exit the script on it.
PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" exists /one.txt >/dev/null 2>&1 \
    && r=1 || r=0
check "  and the removed name is gone" $r
out=$(fsimg fsck 2>&1)
echo "$out" | sed 's/^/  | /' | tail -3
echo "$out" | grep -qiE 'error|wrong|unattached|deleted inode' && r=1 || r=0
check "e2fsck finds nothing wrong with the volume" $r

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
