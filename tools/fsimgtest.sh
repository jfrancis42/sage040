#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fsimgtest.sh - the host's end of the disk, tested.
#
# fsimg.sh is what every Makefile and every test suite uses to put files
# on the machine's disk, so a fault in it does not look like a fault in
# it: it looks like a program that was never installed, or a test that
# read the previous run's file.  Each check here is one thing that has
# already been wrong or could silently be.
#
# EXIT STATUS IS PART OF THE INTERFACE and is checked explicitly.  A
# command that did its work and exited 1 anyway stops every caller that
# chains on && -- which is how `mv` once left two files undeleted and a
# directory unremoved, with no message anywhere.
#
# Nothing here needs root or a loop device.

set -u
cd "$(dirname "$0")/.."
TOP=$(pwd)
F=$TOP/tools/fsimg.sh
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
IMG=$SCRATCH/fsimgtest.img
LOG=$SCRATCH/fsimgtest.log
rm -f "$IMG" "$LOG"

pass=0; fail=0
check() {                       # check "what it establishes" <status>
    if [ "$2" = 0 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "[FAIL] $1"
    fi
}
# Both read the status of the command that ran BEFORE them: `local st=$?`
# expands $? before `local` itself runs, which is the one way to keep it.
# An earlier version passed a literal 1 to check(), so every negative
# control reported a failure whatever happened -- a suite that could not
# pass, which is as useless as one that cannot fail.
ok()   { local st=$?; check "$1" "$st"; }
notok(){ local st=$?; if [ "$st" -ne 0 ]; then st=0; else st=1; fi
         check "$1" "$st"; }

# How many blocks are in use, according to e2fsck rather than to us.
used_blocks() {
    e2fsck -fn "$IMG?offset=$OFF" 2>/dev/null |
        sed -n 's|.*[, ]\([0-9]*\)/[0-9]* blocks.*|\1|p' | tail -1
}

echo "=== fsimg.sh ==="

dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' |
    sfdisk -q "$IMG" >/dev/null 2>&1

"$F" "$IMG" mkfs SAGE040 >>"$LOG" 2>&1
check "mkfs exits 0" $?
OFF=$("$F" "$IMG" offset)
[ "$OFF" = 1048576 ]; check "the partition offset is read from the image" $?

e2fsck -fn "$IMG?offset=$OFF" >>"$LOG" 2>&1
check "a fresh filesystem is clean" $?

BASE=$(used_blocks)
[ -n "$BASE" ]; check "e2fsck reports a block count" $?

# --- files in and out -------------------------------------------------
printf 'one line\n' > "$SCRATCH/a.txt"
# Big enough to need an indirect block at any block size this uses.
head -c 400000 /dev/urandom > "$SCRATCH/big.bin"

"$F" "$IMG" put "$SCRATCH/a.txt" /a.txt; check "put exits 0" $?
"$F" "$IMG" cat /a.txt | grep -qx "one line"
check "a file put in reads back with the same contents" $?

"$F" "$IMG" put "$SCRATCH/big.bin" /big.bin; check "put of a large file exits 0" $?
"$F" "$IMG" get /big.bin "$SCRATCH/back.bin"; check "get exits 0" $?
cmp -s "$SCRATCH/big.bin" "$SCRATCH/back.bin"
check "a file crossing indirect blocks round-trips byte for byte" $?

[ "$("$F" "$IMG" size /big.bin)" = 400000 ]
check "size reports the byte count, not the block count" $?

# --- directories ------------------------------------------------------
"$F" "$IMG" mkdir /etc/deep/deeper; check "mkdir makes missing parents" $?
"$F" "$IMG" isdir /etc/deep/deeper;  check "and each one is a directory" $?
"$F" "$IMG" put "$SCRATCH/a.txt" /etc/deep/deeper/x.txt
check "a file can be put into a directory that had to be made" $?

"$F" "$IMG" ls / | grep -qx "etc";  check "ls lists a directory" $?
"$F" "$IMG" ls / | grep -qx "\.\."; notok "ls does not list . or .."
"$F" "$IMG" ls-l /etc/deep/deeper | grep -q "x.txt"
check "ls-l lists the file with its details" $?
"$F" "$IMG" ls-l / | grep -q "^quit\|^ls -l"
notok "ls-l does not echo debugfs's own commands as output"

# --- permissions ------------------------------------------------------
"$F" "$IMG" put -m 755 "$SCRATCH/a.txt" /prog; check "put -m exits 0" $?
"$F" "$IMG" ls-l / | grep -q "100755.*prog"
check "put -m sets the mode AND keeps the regular-file bits" $?

# --- rename and delete, and their exit status -------------------------
"$F" "$IMG" mv /a.txt /renamed.txt; check "mv exits 0 when it succeeds" $?
"$F" "$IMG" exists /renamed.txt;    check "the new name is there" $?
"$F" "$IMG" exists /a.txt;          notok "the old name is gone"

"$F" "$IMG" rm /renamed.txt /prog;  check "rm of several files exits 0" $?
"$F" "$IMG" exists /renamed.txt;    notok "the first is gone"
"$F" "$IMG" exists /prog;           notok "the second is gone too"

"$F" "$IMG" rm /etc/deep/deeper/x.txt
"$F" "$IMG" rmdir /etc/deep/deeper; check "rmdir exits 0" $?
"$F" "$IMG" exists /etc/deep/deeper; notok "and the directory is gone"

# --- the thing that catches a leak ------------------------------------
# Overwriting has to free what it replaces. Ten rewrites of a 400 KB
# file leak 1 MB if it does not, which is invisible until a build runs
# out of disk for no reason anyone can see.
for i in 1 2 3 4 5 6 7 8 9 10; do
    "$F" "$IMG" put "$SCRATCH/big.bin" /big.bin
done
"$F" "$IMG" rm /big.bin
"$F" "$IMG" rmdir /etc/deep
"$F" "$IMG" rmdir /etc
AFTER=$(used_blocks)
[ "$BASE" = "$AFTER" ]
check "everything put in and taken out again leaves the block count where it started (was $BASE, now $AFTER)" $?

e2fsck -fn "$IMG?offset=$OFF" >>"$LOG" 2>&1
check "the filesystem is still clean after all of that" $?

# --- a tree, put twice ------------------------------------------------
# debugfs's `mkdir` on a name that already exists allocates the inode,
# THEN fails to link it, and leaves it behind unconnected. Installing
# the same tree twice -- which every `make install` does -- leaked one
# inode per directory, and e2fsck called them unconnected directory
# inodes.
mkdir -p "$SCRATCH/rtree/a/b/c"
echo one > "$SCRATCH/rtree/f1"
echo two > "$SCRATCH/rtree/a/f2"
echo three > "$SCRATCH/rtree/a/b/c/f3"
"$F" "$IMG" mkdir /treetest
"$F" "$IMG" put -r "$SCRATCH/rtree" /treetest
INODES1=$(e2fsck -fn "$IMG?offset=$OFF" 2>/dev/null |
          sed -n 's|.*: \([0-9]*\)/[0-9]* files.*|\1|p' | tail -1)
"$F" "$IMG" put -r "$SCRATCH/rtree" /treetest
"$F" "$IMG" put -r "$SCRATCH/rtree" /treetest
INODES2=$(e2fsck -fn "$IMG?offset=$OFF" 2>/dev/null |
          sed -n 's|.*: \([0-9]*\)/[0-9]* files.*|\1|p' | tail -1)
[ "$INODES1" = "$INODES2" ]
check "putting the same tree in three times uses the inodes of one (was $INODES1, now $INODES2)" $?

"$F" "$IMG" cat /treetest/a/b/c/f3 | grep -qx three
check "  and the deepest file in it is still right" $?

e2fsck -fn "$IMG?offset=$OFF" >>"$LOG" 2>&1
check "  and e2fsck finds no unconnected directory" $?

# --- negative controls ------------------------------------------------
# A suite that cannot fail is measuring nothing.
"$F" "$IMG" exists /never-existed;  notok "exists reports a missing file as missing"
"$F" "$IMG" put /no/such/host/file /x >/dev/null 2>&1
notok "put of a source that is not there fails"
"$F" "$IMG" cat /never-existed >/dev/null 2>&1
notok "cat of a missing file fails"
"$F" "$IMG" isdir /never-existed;   notok "isdir of a missing path fails"

# The leak check must be able to see a leak: plant one and look.
"$F" "$IMG" put "$SCRATCH/big.bin" /leak.bin
LEAKED=$(used_blocks)
[ "$BASE" = "$LEAKED" ]
notok "the leak check notices blocks that were not given back"
"$F" "$IMG" rm /leak.bin

echo
echo " passed: $pass"
echo " failed: $fail"
if [ "$fail" = 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
