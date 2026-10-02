#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# mounttest.sh - more than one volume: mount(2), umount2(2), mount(8).
#
# A three-partition disk: hda1 the root, hda2 an ext2 volume with a
# journal, hda3 a small one without. libc/test/mnttest does most of the
# work on the machine (see the head of it); this script builds the disk,
# runs it, and then judges what it left with the HOST's tools:
#
#   - all three volumes clean by e2fsck after the machine halts, which
#     is what proves halt unmounted every one of them and not only the
#     root;
#   - the files written on hda2 are on hda2, read back by debugfs and
#     (with sudo) by Linux itself;
#   - hda3, mounted read-only, is BIT FOR BIT what it was before the
#     machine ever saw it. A read-only mount that writes even its
#     superblock's mount time fails this, which is the point of it.
#
# Then the journal on a volume that is not the root: the machine is
# stopped after the commit block of a sync on hda2 (the KSTAT_JOURNAL_
# STOP knob journaltest uses), so the new file is only in hda2's log; the
# next boot's mount must replay it.

set -u
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-mount.img"
P1=2048;   S1=63488             # 31 MB: the root
P2=65536;  S2=40960             # 20 MB: ext2 with a journal
P3=106496; S3=24576             # 12 MB: ext2, no journal; mounted ro
LOG="$SCRATCH/mounttest.log"; FIFO="$SCRATCH/mount.fifo"
WORK="$SCRATCH/mount.tmp"
rm -rf "$LOG" "$FIFO" "$WORK"; mkdir -p "$WORK"
fsimg_at(){ local o=$1; shift; PART_OFFSET=$((o * 512)) ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "mounttest: fsimg $* failed" >&2; exit 1; }; }
fsimg(){ fsimg_at $P1 "$@"; }

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
make -s -C ../libc/test mnttest jtest >/dev/null 2>&1 || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
printf 'label: dos\nunit: sectors\nstart=%d, size=%d, type=83\nstart=%d, size=%d, type=83\nstart=%d, size=%d, type=83\n' \
    $P1 $S1 $P2 $S2 $P3 $S3 | sfdisk -q "$DISK" >/dev/null
mkvol() {                       # mkvol START SECTORS LABEL [-j]
    mke2fs -q -t ext2 ${4:-} -b 4096 -I 256 -O ^dir_index,^resize_inode \
        -L "$3" -E offset=$(($1 * 512)) -F "$DISK" $(($2 / 8)) ||
        { echo "mounttest: mke2fs $3 failed" >&2; exit 1; }
}
mkvol $P1 $S1 SAGE040 -j
mkvol $P2 $S2 DATA -j
mkvol $P3 $S3 RODISK
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /tmp
fsimg mkdir /mnt; fsimg mkdir /ro
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
put_shells
for p in cat ls true; do fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p; done
for p in mount umount df; do fsimg put -m 755 "../system/$p" /bin/$p; done
fsimg put -m 755 ../libc/test/mnttest /mnttest
fsimg put -m 755 ../libc/test/jtest /jtest
printf 'under the mount\n' > "$WORK/shadow.txt"; fsimg put "$WORK/shadow.txt" /mnt/shadow.txt
printf 'hello from hda2\n' > "$WORK/hello.txt"; fsimg_at $P2 put "$WORK/hello.txt" /hello.txt
printf 'read only\n' > "$WORK/ro.txt";         fsimg_at $P3 put "$WORK/ro.txt" /ro.txt
p3sum() { dd if="$DISK" bs=512 skip=$P3 count=$S3 status=none | md5sum | cut -d' ' -f1; }
P3_BEFORE=$(p3sum)

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
    wait_for 'kernel ready.*' 600 || echo "    (no boot)"
    sleep 0.5
}
wait_for() {                    # wait_for REGEX [TENTHS]: a WHOLE line
    local i
    for i in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG.cur" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() {                         # run TAG COMMAND
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" "${3:-1200}" || echo "    (timed out: $1)"
}
send() { printf '%s\r' "$1" >&3; }
stop() {
    exec 3>&-
    kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    tr -d '\r' < "$LOG.cur" >> "$LOG"
    rm -f "$FIFO"
}
halt_vm() {
    send halt
    wait_for 'halting.*' 300
    sleep 1
    stop
}
said() { tr -d '\r' < "$LOG.cur" | grep -aqE -- "$1"; }
status_of() { tr -d '\r' < "$LOG.cur" | sed -n "s/^$1-END //p" | tail -1; }

# --- the host's view --------------------------------------------------
vol() { dd if="$DISK" of="$2" bs=512 skip="$1" count="$3" status=none; }
# clean START SECTORS: e2fsck -fn says nothing, AND the superblock says
# the volume was unmounted. -f checks a volume whatever its state, so
# e2fsck alone would pass one that was never unmounted at all -- the
# state field and needs_recovery are what say it was let go properly.
clean() {
    local p="$WORK/vol.part"
    vol "$1" "$p" "$2"
    e2fsck -fn "$p" > "$WORK/fsck.out" 2>&1 &&
        ! grep -qE 'Fix\?|count wrong|differences|Unconnected|Unattached|needs_recovery|recovering' "$WORK/fsck.out" &&
        debugfs -R stats "$p" 2>/dev/null > "$WORK/stats.out" &&
        grep -qE '^Filesystem state: +clean$' "$WORK/stats.out" &&
        ! grep -q needs_recovery "$WORK/stats.out"
}
dcat() { debugfs -R "cat $2" "$DISK?offset=$(($1 * 512))" 2>/dev/null; }

pass=0; fail=0; skip=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
skipped() { echo "  [SKIP] $1"; skip=$((skip + 1)); }
expect_text() {
    local i; for ((i = 0; i < $2; i++)); do printf '%s line %d of the journal test\n' "$1" "$i"; done
}

# --- 1. the program ---------------------------------------------------
boot
said 'hda .*3 partitions'
check "the boot banner finds the disk's three partitions" $?
run MNT "/mnttest" 3000
run LIST "/bin/mount"
run DF "/bin/df"
run DFP "/bin/df /mnt/d"
halt_vm
CUR="$WORK/s1.log"; tr -d '\r' < "$LOG" > "$CUR"
echo "=== session 1 ==="
sed -n '/^mnttest: start/,/^DFP-END/p' "$CUR" | sed 's/^/  | /'

echo "=== checks: on the machine ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*) check "${line#  ok   }" 0 ;;
        "  FAIL "*) check "${line#  FAIL }" 1 ;;
    esac
done < <(sed -n '/^mnttest: start/,/^mnttest: [0-9]* checks/p' "$CUR")
grep -qE '^mnttest: [0-9]+ checks, 0 failed$' "$CUR"
check "mnttest ran to the end, nothing failed" $?
grep -qx '/dev/hda2 /mnt ext3 rw 0 0' "$CUR"
check "mount(8) with no arguments lists hda2 on /mnt, as ext3: it has a journal" $?
grep -qE '^SAGE040 .*%  /$' "$CUR" && grep -qE '^DATA .*%  /mnt$' "$CUR"
check "df prints a line for each volume, by label, with where it is mounted" $?
# From the line that typed it (prompt and all) to its end marker.
[ "$(sed -n '/df \/mnt\/d;/,/^DFP-END/p' "$CUR" | grep -cE '%  /mnt$')" = 1 ] &&
    ! sed -n '/df \/mnt\/d;/,/^DFP-END/p' "$CUR" | grep -qE '%  /$'
check "  and df PATH gives the volume that path is on" $?

echo "=== checks: the host's view after halt ==="
clean $P1 $S1; check "hda1 (the root) is clean by e2fsck" $?
clean $P2 $S2; check "hda2 is clean by e2fsck: halt unmounted it too" $?
clean $P3 $S3; check "hda3 is clean by e2fsck" $?
[ "$(p3sum)" = "$P3_BEFORE" ]
check "hda3, mounted read-only, is bit for bit what it was" $?
[ "$(dcat $P2 /final.txt)" = "written before halt" ]
check "the file written last is on hda2 (debugfs)" $?
[ "$(dcat $P2 /d/rel.txt)" = "relative" ] && [ "$(dcat $P2 /new.txt)" = "new on hda2" ]
check "  and the others, where they were made" $?
[ "$(dcat $P1 /mnt/shadow.txt)" = "under the mount" ] &&
    ! debugfs -R "stat /final.txt" "$DISK?offset=$((P1 * 512))" 2>/dev/null | grep -q Inode:
check "  and none of it leaked onto the root under /mnt" $?
if sudo -n true 2>/dev/null && command -v losetup >/dev/null; then
    m="$WORK/linuxmnt"; mkdir -p "$m"; vol $P2 "$WORK/p2.img" $S2
    ok=1
    if sudo -n mount -t ext3 -o loop,ro "$WORK/p2.img" "$m" 2>/dev/null; then
        [ "$(sudo -n cat "$m/final.txt")" = "written before halt" ] &&
            [ "$(sudo -n stat -c %h "$m/new.txt")" = 2 ] && ok=0
        sudo -n umount "$m"
    fi
    check "LINUX mounts hda2 and finds the file, and the hard link's count of 2" $ok
else
    skipped "Linux mounting hda2 (needs sudo and losetup)"
fi

# --- 2. the journal of a volume that is not the root ------------------
echo "=== checks: the journal on hda2 ==="
boot
run M2 "/bin/mount /dev/hda2 /mnt"
[ "$(status_of M2)" = 0 ]; check "mounted again" $?
send "/jtest sync; /jtest stop 257 1; /jtest write /mnt/a.txt 300; /jtest sync"
wait_for 'journal: stopped after the commit block' 600
check "the machine stops after hda2's commit block" $?
stop
! debugfs -R "stat /a.txt" "$DISK?offset=$((P2 * 512))" 2>/dev/null | grep -q 'Inode:'
check "  the new file is only in hda2's log, not on the volume" $?
! clean $P2 $S2
check "  and the host's check calls hda2 NOT clean -- so the checks above can fail" $?
boot
run M3 "/bin/mount /dev/hda2 /mnt"
[ "$(status_of M3)" = 0 ]; check "mounting it replays the log" $?
run CHK "/jtest check /mnt/a.txt 300"
[ "$(status_of CHK)" = 0 ]; check "  and the file is there, all 300 lines" $?
halt_vm
clean $P2 $S2; check "hda2 is clean by e2fsck afterwards" $?
expect_text /mnt/a.txt 300 > "$WORK/a.want"
dcat $P2 /a.txt | cmp -s - "$WORK/a.want"
check "  and debugfs reads the file byte for byte" $?
clean $P1 $S1; check "the root stayed clean through all of it" $?

tr -d '\r' < "$LOG" | grep -qE 'panic|bus error|address error|DOUBLE MMU FAULT'
[ $? -ne 0 ]; check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed, $skip skipped ---"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
