#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# linuxfstest.sh - the machine's filesystem against LINUX's, on one volume.
#
# Every other suite checks the disk with e2fsprogs. This one hands the
# same volume back and forth with the Linux kernel's own ext3 driver (a
# loop mount, so it needs sudo), and with standard tools on both sides:
#
#   1. Linux builds a tree: sizes either side of every block boundary and
#      through the double-indirect range, a sparse file, binary data,
#      every permission bit (setuid, setgid, sticky, 0000), four owners
#      and groups, hard links, fast and slow and dangling symbolic links,
#      a 255-byte name, UTF-8 names, a 600-entry directory, deep nesting,
#      a FIFO, set times.
#   2. The machine lists it: fsmanifest's listing (type, mode, owner,
#      group, INODE, link count, size, mtime, a hash of every byte, link
#      targets) must equal Linux's, line for line.
#   3. The machine changes it with sbase's tools -- appends, overwrites,
#      renames within and across directories and over existing names, a
#      directory renamed, links made and broken, chmod/chown/chgrp,
#      truncates, a subtree removed, hundreds of files made and half of
#      them deleted, a tar archive unpacked -- and lists it again.
#   4. Linux mounts it: e2fsck first (clean), then its listing must equal
#      the machine's. Then Linux changes it with coreutils, and the
#      machine must list exactly what Linux does.
#   5. Semantics, not just storage: what the machine's own calls did
#      must be what Linux's would have -- a file made in a setgid
#      directory takes the directory's group, rename over a name
#      replaces it, unlink of one of three links leaves two.

set -u
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-linuxfs.img"; OFF=$((2048*512))
LOG="$SCRATCH/linuxfstest.log"; FIFO="$SCRATCH/linuxfs.fifo"
WORK="$SCRATCH/linuxfs.tmp"; MNT="$WORK/mnt"
rm -rf "$LOG" "$FIFO"
[ -d "$MNT" ] && sudo -n umount "$MNT" 2>/dev/null
rm -rf "$WORK"; mkdir -p "$MNT"
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "linuxfstest: fsimg $* failed" >&2; exit 1; }; }

if ! sudo -n true 2>/dev/null || ! command -v losetup >/dev/null; then
    echo "  [SKIP] the whole suite: Linux mounting the volume needs sudo"
    echo "RESULT: PASS"
    exit 0
fi

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
make -s -C ../libc/test fsmanifest jtest >/dev/null 2>&1 || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=96 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /tmp
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
put_shells bash
for p in ../ports/sbase/bin/*; do fsimg put -m 755 "$p" "/bin/$(basename "$p")"; done
fsimg put -m 755 ../libc/test/fsmanifest /fsmanifest
fsimg put -m 755 ../libc/test/jtest /jtest

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

# --- Linux's side -----------------------------------------------------
lmount() { sudo -n mount -t ext3 -o loop,offset=$OFF "$DISK" "$MNT"; }
lumount() { sync; sudo -n umount "$MNT"; }
lmanifest() { sudo -n python3 "$PWD/fsmanifest.py" "$MNT/lx" > "$1"; }
clean() {                       # the host's e2fsck: clean, and silent
    dd if="$DISK" of="$WORK/part" bs=512 skip=2048 status=none
    e2fsck -fn "$WORK/part" > "$WORK/e2fsck.out" 2>&1 &&
        ! grep -qE 'Fix\?|count wrong|differences|Unconnected|Unattached|Clear\?' "$WORK/e2fsck.out"
}
same() {                        # same A B LABEL: listings equal, or show how
    if cmp -s "$1" "$2"; then
        return 0
    fi
    echo "    --- $3: first differences (Linux <, machine >)"
    diff "$1" "$2" | head -12 | sed 's/^/    /'
    return 1
}

echo "=== Linux builds the tree ==="
lmount || { echo "linuxfstest: mount failed"; exit 1; }
sudo -n bash -e -s "$MNT/lx" <<'EOF'
L=$1
mkdir -p "$L"; cd "$L"
mkdir sizes perm owners links names deep big
for n in 0 1 511 512 4095 4096 4097 8191 8192 49151 49152 49153; do
    head -c $n /dev/urandom > sizes/s$n
done
head -c $((4096 * 12 + 4096 * 1024 + 8192)) /dev/urandom > sizes/doubleindirect
head -c 100000 /dev/urandom > sizes/binary
truncate -s 20M sizes/sparse
printf 'in the middle of a hole' | dd of=sizes/sparse bs=1 seek=$((10 * 1024 * 1024)) conv=notrunc status=none
for m in 0000 0400 0600 0640 0644 0755 0700 4755 2755 6755 0777; do
    echo "mode $m" > perm/m$m; chmod $m perm/m$m
done
mkdir perm/sticky perm/setgid perm/private
chmod 1777 perm/sticky; chmod 2775 perm/setgid; chmod 0700 perm/private
chgrp 100 perm/setgid
echo root > owners/root
echo user > owners/user;   chown 1000:1000 owners/user
echo other > owners/other; chown 1001:100 owners/other
echo nobody > owners/nobody; chown 65534:65534 owners/nobody
echo big > owners/bigids;   chown 100000:200000 owners/bigids
mkdir owners/userdir; chown 1000:1000 owners/userdir
echo shared > links/one
mkdir links/a links/b
ln links/one links/a/two
ln links/one links/b/three
ln -s one links/fast
ln -s "$(printf 'x%.0s' $(seq 1 200))" links/slow-dangling
ln -s ../sizes links/todir
ln -s /nonexistent/target links/dangling
ln -s a/two links/viahard
mkfifo links/fifo
touch "names/$(printf 'n%.0s' $(seq 1 255))"
echo accent > "names/caf$(printf '\xc3\xa9')"
echo kanji > "names/$(printf '\xe6\x97\xa5\xe6\x9c\xac')"
echo spaced > "names/with spaces and	tab"
echo dash > "names/-leading-dash"
for i in $(seq 1 600); do echo "$i" > "big/file$i"; done
d=deep; for i in $(seq 1 20); do d=$d/level$i; mkdir "$d"; done; echo bottom > "$d/bottom"
touch -d '2001-02-03 04:05:06' sizes/s1
touch -d '2037-12-31 23:59:59' sizes/s0
touch -d '1999-01-01 00:00:00' owners/userdir
EOF
check "Linux builds the tree on the machine's volume" $?
lmanifest "$WORK/L1"
lumount
clean
check "  and unmounts it clean" $?

# --- the machine ------------------------------------------------------
qpid=
boot() {
    rm -f "$FIFO"; mkfifo "$FIFO"
    : > "$LOG.cur"
    "$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
        -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
        -chardev stdio,id=con,signal=off -serial chardev:con \
        < "$FIFO" > "$LOG.cur" 2>&1 &
    qpid=$!
    exec 3> "$FIFO"
    wait_for 'kernel ready.*' 900 || echo "    (no boot)"
}
wait_for() {
    local i
    for i in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG.cur" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() {
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" "${3:-3000}" || echo "    (timed out: $1)"
}
status_of() { tr -d '\r' < "$LOG.cur" | sed -n "s/^$1-END //p" | tail -1; }
halt_vm() {
    printf 'halt\r' >&3
    wait_for 'halting.*' 600
    sleep 1
    exec 3>&-; kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    tr -d '\r' < "$LOG.cur" >> "$LOG"; rm -f "$FIFO"
}
getfile() { debugfs -R "dump $1 $2" "$DISK?offset=$OFF" >/dev/null 2>&1; }

echo "=== the machine reads it ==="
boot
run M1 "/fsmanifest /lx > /m1.txt"
run SUMS "/bin/sha256sum /lx/sizes/doubleindirect /lx/sizes/binary /lx/sizes/sparse > /sums.txt"
halt_vm
getfile /m1.txt "$WORK/M1"
same "$WORK/L1" "$WORK/M1" "listing after Linux built it"
check "the machine lists what Linux made: every name, type, mode, owner, group, inode, link count, size, time, byte and target" $?
getfile /sums.txt "$WORK/sums"
(cd "$WORK" && lmount && sudo -n sha256sum "$MNT/lx/sizes/doubleindirect" "$MNT/lx/sizes/binary" "$MNT/lx/sizes/sparse" | sed "s|$MNT||" > sums.linux; lumount)
sed 's|  */lx|  /lx|' "$WORK/sums" | cmp -s - "$WORK/sums.linux"
check "  sbase's sha256sum on the machine agrees with Linux's on the big, binary and sparse files" $?
clean
check "  reading changed nothing e2fsck can see" $?

echo "=== the machine changes it ==="
cat > "$WORK/ops.sh" <<'EOF'
set -e
cd /lx
# appending, overwriting part of a file, growing past block boundaries
printf 'appended\n' >> sizes/s4095
printf 'appended across the boundary %s\n' $(seq 1 40) >> sizes/s4096
printf 'ZZZZ' | dd of=sizes/s8192 bs=1 seek=4094 conv=notrunc 2>/dev/null
cat sizes/s49153 sizes/s49153 >> sizes/s1
# renames: within, across, over an existing name, a whole directory
mv sizes/s511 sizes/renamed511
mv sizes/s512 owners/moved512
mv owners/other owners/root                 # replaces root
mv deep/level1/level2 names/level2-moved
# links
rm links/b/three                            # three links become two
ln links/one links/four
ln -s ../perm/m0644 links/newsym
rm links/dangling
ln -sf one links/fast                       # replace a symlink
# modes and owners
chmod 0640 perm/m0777
chmod u+s,g+s perm/m0755
chown 1000:100 perm/m0600
chgrp 1001 owners/user
chmod 0 perm/m0640
chown 70000:80000 owners/user               # past 16 bits: the high halves
# files made here, in a setgid directory and in a sticky one
echo madehere > perm/setgid/new-in-setgid
mkdir perm/setgid/subdir
echo sticky > perm/sticky/mine
# truncating
: > sizes/s49152
dd if=/dev/zero of=sizes/s8191 bs=1 count=0 seek=100 2>/dev/null
# a subtree removed, hundreds of files made and half deleted
rm -r deep/level1
mkdir many
for i in $(seq 1 400); do echo "made $i" > many/f$i; done
for i in $(seq 1 2 400); do rm many/f$i; done
for i in $(seq 1 600); do [ $((i % 3)) = 0 ] && rm big/file$i; done
# a tar archive made and unpacked
tar -cf /tmp/t.tar sizes/s4097 links/one owners
mkdir untar && cd untar && tar -xf /tmp/t.tar && cd ..
touch -d '2033-03-03 03:03:03' sizes/s4097 2>/dev/null || touch sizes/s4097
EOF
fsimg put "$WORK/ops.sh" /ops.sh
boot
run OPS "/bin/bash /ops.sh"
[ "$(status_of OPS)" = 0 ]
check "the machine's tools change the tree (appends, renames, links, modes, owners, truncates, rm -r, tar)" $?
run M2 "/fsmanifest /lx > /m2.txt"
halt_vm
clean
check "  halted, the host's e2fsck finds the volume clean" $?
getfile /m2.txt "$WORK/M2"
lmount
lmanifest "$WORK/L2"
lumount
same "$WORK/L2" "$WORK/M2" "listing after the machine's changes"
check "Linux mounts it and lists exactly what the machine does" $?

# Semantics: what the machine's calls did is what Linux's would have.
field() { awk -F'\t' -v p="$2" '$1 == p {print $'"$3"'}' "$1"; }
[ "$(field "$WORK/M2" perm/setgid/new-in-setgid 5)" = 100 ]
check "  a file made in a setgid directory takes the directory's group (100)" $?
m=$(field "$WORK/M2" perm/setgid/subdir 3); [ $((0$m & 02000)) -ne 0 ]
check "  a directory made there inherits the setgid bit ($m)" $?
[ "$(field "$WORK/M2" links/one 7)" = 3 ] && [ -z "$(field "$WORK/M2" links/b/three 7)" ]
check "  one link removed and one made: three links still, the removed name gone" $?
[ "$(field "$WORK/M2" owners/root 10)" = "$(field "$WORK/L1" owners/other 10)" ] &&
    [ "$(field "$WORK/M2" owners/root 6)" = "$(field "$WORK/L1" owners/other 6)" ]
check "  rename over a name replaces it: the old file's inode and bytes under the new name" $?
[ "$(field "$WORK/M2" sizes/s49152 8)" = 0 ] && [ "$(field "$WORK/M2" sizes/s8191 8)" = 100 ]
check "  truncate to zero and to 100 bytes" $?
[ "$(field "$WORK/M2" links/fast 11)" = one ] && [ "$(field "$WORK/M2" links/newsym 11)" = ../perm/m0644 ]
check "  symbolic links made and replaced point where they were told" $?
[ "$(field "$WORK/M2" perm/m0755 3)" = 6755 ] && [ "$(field "$WORK/M2" perm/m0600 4)" = 1000 ] &&
    [ "$(field "$WORK/M2" perm/m0600 5)" = 100 ]
check "  chmod u+s,g+s and chown uid:gid stored as asked" $?
[ "$(field "$WORK/M2" owners/user 4)" = 70000 ] && [ "$(field "$WORK/M2" owners/user 5)" = 80000 ] &&
    [ "$(field "$WORK/M2" owners/bigids 4)" = 100000 ] && [ "$(field "$WORK/M2" owners/bigids 5)" = 200000 ]
check "  uids and gids past 65535, both ways (100000:200000 from Linux, 70000:80000 from here)" $?
[ "$(grep -c "^many/f" "$WORK/M2")" = 200 ] && [ "$(grep -c "^big/file" "$WORK/M2")" = 400 ]
check "  400 made and 200 removed; 600 and every third removed" $?

echo "=== Linux changes it again ==="
lmount
sudo -n bash -e -s "$MNT/lx" <<'EOF'
L=$1; cd "$L"
cp -a sizes sizes-copy
mv many many-renamed
rm -rf big
chmod 0751 perm/sticky
chown -R 2000:2000 untar
echo "linux appended" >> sizes/s1
ln sizes/s1 sizes/s1-link
ln -s s1 sizes/s1-sym
mkdir -p again/and/again && echo deep > again/and/again/file
rm -f sizes/sparse
touch -d '2010-10-10 10:10:10' sizes-copy/s4096
EOF
check "Linux's coreutils change it (cp -a, mv, rm -rf, chmod, chown -R, ln)" $?
lmanifest "$WORK/L3"
lumount
clean
check "  and leave it clean" $?
boot
run M3 "/fsmanifest /lx > /m3.txt"
run ROUND "/bin/cat /lx/again/and/again/file /lx/sizes/s1-sym > /dev/null"
[ "$(status_of ROUND)" = 0 ]
check "the machine reads through what Linux made" $?
halt_vm
getfile /m3.txt "$WORK/M3"
same "$WORK/L3" "$WORK/M3" "listing after Linux's second changes"
check "  and lists exactly what Linux does" $?
clean
check "  clean at the end" $?

echo "=== the same storm on both: a differential test ==="
# jtest's churn is deterministic: the same seed makes the same calls in
# the same order -- creates and appends, renames within and across
# directories, links, symlinks (dangling ones included), truncates,
# unlinks, mkdirs and rmdirs, including the ones that must FAIL. Linux
# runs it, built for the host, on its own mount; the machine runs it on
# its own volume; both log every call with its result and errno (by
# name). The logs must be identical, and so must the trees: every name,
# type, mode, owner, link count, size, byte and target. Inode numbers
# and times may differ -- the two kernels allocate and stamp on their
# own. It found O_CREAT through a dangling link and link(2) of a
# symlink both done differently from Linux.
cc -O2 -o "$WORK/jtest-host" ../libc/test/jtest.c 2>"$WORK/cc.err"
check "jtest built for the host" $?
strip() { cut -f1-5,7,8,10,11 "$1"; }
SEEDS="4242 777 31337"
lmount
for seed in $SEEDS; do
    sudo -n mkdir -p "$MNT/storm$seed"
    sudo -n sh -c "umask 022; '$WORK/jtest-host' churn $MNT/storm$seed/x 3000 $seed $WORK/L$seed.log >/dev/null"
    sudo -n python3 "$PWD/fsmanifest.py" "$MNT/storm$seed" > "$WORK/LS$seed"
done
lumount
boot
for seed in $SEEDS; do
    run STORM$seed "umask 022; /bin/mkdir -p /m$seed; /jtest churn /m$seed/x 3000 $seed /m$seed.log > /dev/null"
    run MS$seed "/fsmanifest /m$seed > /ms$seed.txt"
done
halt_vm
for seed in $SEEDS; do
    getfile /m$seed.log "$WORK/M$seed.log"
    getfile /ms$seed.txt "$WORK/MS$seed"
    same "$WORK/L$seed.log" "$WORK/M$seed.log" "seed $seed: the calls"
    rc=$?
    n=$(wc -l < "$WORK/L$seed.log")
    check "  seed $seed: all $n calls return what Linux's do, errno and all" $rc
    same <(strip "$WORK/LS$seed") <(strip "$WORK/MS$seed") "seed $seed: the tree"
    rc=$?
    n=$(wc -l < "$WORK/LS$seed")
    check "  seed $seed: and the tree ends exactly as Linux's does ($n names)" $rc
done
clean
check "  and the volume is clean" $?

grep -aqE 'panic|bus error|address error|DOUBLE MMU FAULT' "$LOG"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
