#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# journaltest.sh - the ext3 journal, by pulling the plug.
#
# The machine is stopped at the two moments that matter, with a kernel
# knob (kstat KSTAT_JOURNAL_STOP) rather than by luck:
#
#   COMMITTED   after a transaction's commit block, before any of it is
#               written home. The change is then ONLY in the log -- the
#               host's debugfs, which ignores the journal, cannot see it
#               -- and replaying must bring it back, intact.
#   UNCOMMITTED just before the commit block. Replaying must leave it
#               out, and the volume as it was.
#
# Each is replayed three ways that share no code: by the HOST's e2fsck,
# by Linux mounting the volume (when sudo is available), and by this
# kernel at its next boot. Then the machine is killed at random moments
# in a storm of creates, renames, links, truncates and unlinks, and every
# one of those volumes must need nothing from e2fsck but the journal.
# The control: the same storm on a volume WITHOUT a journal, killed the
# same way, must leave damage e2fsck finds -- or the e2fsck checks above
# would be proving nothing.

set -u
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-journal.img"; OFF=$((2048*512))
LOG="$SCRATCH/journaltest.log"; FIFO="$SCRATCH/journal.fifo"
WORK="$SCRATCH/journal.tmp"
rm -rf "$LOG" "$FIFO" "$WORK"; mkdir -p "$WORK"
fsimg_on(){ local d=$1; shift; PART_OFFSET=$OFF ../tools/fsimg.sh "$d" "$@"; }
fsimg(){ fsimg_on "$DISK" "$@" || { echo "journaltest: fsimg $* failed" >&2; exit 1; }; }

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
make -s -C ../libc/test jtest fsmanifest >/dev/null 2>&1 || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

newdisk() {                     # newdisk IMG [nojournal]
    rm -f "$1"
    dd if=/dev/zero of="$1" bs=1M count=48 status=none
    printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$1" >/dev/null
    if [ "${2:-}" = nojournal ]; then
        FS_JOURNAL=0 fsimg_on "$1" mkfs SAGE040 >/dev/null
    else
        fsimg_on "$1" mkfs SAGE040 >/dev/null
    fi
    DISK=$1
    fsimg put kernel.rom /KERNEL.ROM
    fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /tmp
    fsimg put ../ldso/ld.so /lib/ld.so
    fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
    put_shells bash
    for p in cat ls sleep true; do fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p; done
    fsimg put -m 755 ../libc/test/jtest /jtest
    fsimg put -m 755 ../libc/test/fsmanifest /fsmanifest
}

# --- the machine ------------------------------------------------------
qpid=
boot() {                        # boot IMG
    rm -f "$FIFO"; mkfifo "$FIFO"
    : > "$LOG.cur"
    "$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
        -drive file="$1",format=raw,if=ide -display none -no-reboot -nic none \
        -chardev stdio,id=con,signal=off -serial chardev:con \
        < "$FIFO" > "$LOG.cur" 2>&1 &
    qpid=$!
    exec 3> "$FIFO"
    wait_for 'kernel ready.*' 600 || echo "    (no boot)"
}
wait_for() {                    # wait_for REGEX [TENTHS]
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
stop() {                        # pull the plug
    exec 3>&-
    kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
    tr -d '\r' < "$LOG.cur" >> "$LOG"
    rm -f "$FIFO"
}
halt_vm() {                     # stop it properly
    send halt
    wait_for 'halting.*' 300
    sleep 1
    stop
}
said() { tr -d '\r' < "$LOG.cur" | grep -aqE -- "$1"; }
status_of() { tr -d '\r' < "$LOG.cur" | sed -n "s/^$1-END //p" | tail -1; }

# --- the host's view --------------------------------------------------
part() { dd if="$1" of="$2" bs=512 skip=2048 status=none; }   # the volume alone
fsck_fix() {                    # e2fsck -fy on a copy; prints its output
    local p="$WORK/fix.part"
    part "$1" "$p"
    e2fsck -fy "$p" > "$WORK/fix.out" 2>&1
    echo $? > "$WORK/fix.status"
    cp "$p" "$WORK/fixed.part"
}
only_journal_fixed() {          # e2fsck -fy fixed nothing but the journal
    ! grep -qE '\? yes|FIXED|Fix\?|Clear\?|Salvage\?|Relocate\?|Connect to' "$WORK/fix.out" &&
        [ "$(cat "$WORK/fix.status")" -le 1 ]
}
fixed_clean() {                 # and a second look finds nothing at all
    e2fsck -fn "$WORK/fixed.part" > "$WORK/fix2.out" 2>&1 &&
        ! grep -qE 'Fix\?|count wrong|differences|Unconnected|Unattached' "$WORK/fix2.out"
}
dbg() { debugfs -R "$1" "$2" 2>/dev/null; }

pass=0; fail=0; skip=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
skipped() { echo "  [SKIP] $1"; skip=$((skip + 1)); }
expect_text() {                 # what `jtest write PATH N` puts in PATH
    local i; for ((i = 0; i < $2; i++)); do printf '%s line %d of the journal test\n' "$1" "$i"; done
}
SUDO=0
sudo -n true 2>/dev/null && command -v losetup >/dev/null && SUDO=1
linux_replays() {               # linux_replays IMG PATH WANTFILE: Linux mounts a copy
    local c="$WORK/linux.img" m="$WORK/mnt" ok=1
    cp "$1" "$c"; mkdir -p "$m"
    if sudo -n mount -t ext3 -o loop,offset=$OFF "$c" "$m" 2>"$WORK/mount.err"; then
        if [ -n "$2" ]; then
            sudo -n cat "$m$2" > "$WORK/linux.cat" 2>/dev/null && cmp -s "$WORK/linux.cat" "$3" && ok=0
        else
            ok=0
        fi
        sudo -n umount "$m"
    fi
    [ $ok -eq 0 ] && { part "$c" "$WORK/linux.part"; e2fsck -fn "$WORK/linux.part" >/dev/null 2>&1; }
}

echo "=== checks ==="
newdisk "$DISK"
dbg stats "$DISK?offset=$OFF" | grep -q 'has_journal'
check "the volume is made with a journal (has_journal)" $?

# --- A: stopped after the commit block --------------------------------
boot "$DISK"
run STATS "/jtest stats"
said '^journal on=1 '
check "the kernel uses the journal (jtest stats: on=1)" $?
# 257 = JSTOP_COMMITTED | JSTOP_SYNC_ONLY: the stop is the commit the
# final sync makes. A transaction more than five seconds old commits at
# the end of whatever call is running -- under load, the open that made
# the file -- and stopping there would capture it empty, which is a
# consistent state but not the one this checks.
send "/jtest sync; /jtest stop 257 1; /jtest write /a.txt 300; /jtest sync"
wait_for 'journal: stopped after the commit block' 300
check "the machine stops after the commit block, before anything goes home" $?
stop
cp "$DISK" "$WORK/A.img"
expect_text /a.txt 300 > "$WORK/a.want"
! dbg "stat /a.txt" "$WORK/A.img?offset=$OFF" | grep -q 'Inode:'
check "  the new file is nowhere on the volume itself: only in the log" $?
dbg logdump "$WORK/A.img?offset=$OFF" | grep -qE 'Found expected sequence|Journal starts at block [1-9]'
check "  debugfs's logdump finds the transaction" $?
fsck_fix "$WORK/A.img"
grep -q 'recovering journal' "$WORK/fix.out" && only_journal_fixed
check "the HOST's e2fsck replays it, and needs to fix nothing else" $?
fixed_clean
check "  and a second e2fsck finds the volume clean" $?
dbg "cat /a.txt" "$WORK/fixed.part" | cmp -s - "$WORK/a.want"
check "  the file is there, byte for byte (300 lines)" $?
if [ $SUDO = 1 ]; then
    linux_replays "$WORK/A.img" /a.txt "$WORK/a.want"
    check "LINUX mounting the volume replays it too: the file, and e2fsck clean after" $?
else
    skipped "Linux replaying it (needs sudo and losetup)"
fi
boot "$DISK"
run STATS2 "/jtest stats"
said '^journal on=1 commits=[0-9]+ forced=[0-9]+ replayed=1$'
check "THIS kernel replays it at boot (replayed=1)" $?
run CHECKA "/jtest check /a.txt 300"
[ "$(status_of CHECKA)" = 0 ]
check "  and the file reads back, every line" $?
! said 'not cleanly unmounted; checked'
check "  without the boot-time check: the journal is why it is not needed" $?
halt_vm
fsimg_on "$DISK" fsck >/dev/null 2>&1
check "  halted, the host's e2fsck finds the volume clean" $?

# --- B: stopped before the commit block --------------------------------
boot "$DISK"
send "/jtest sync; /jtest stop 258 1; /jtest write /b.txt 300; /jtest sync"
wait_for 'journal: stopped before the commit block' 300
check "the machine stops just before a commit block" $?
stop
cp "$DISK" "$WORK/B.img"
fsck_fix "$WORK/B.img"
only_journal_fixed && fixed_clean
check "  e2fsck: nothing to fix, an uncommitted transaction is not one" $?
! dbg "stat /b.txt" "$WORK/fixed.part" | grep -q 'Inode:' &&
    dbg "cat /a.txt" "$WORK/fixed.part" | cmp -s - "$WORK/a.want"
check "  the half-written change is absent, and what came before intact" $?
boot "$DISK"
run CHECKB "/jtest check /b.txt 300"
run CHECKA2 "/jtest check /a.txt 300"
[ "$(status_of CHECKB)" != 0 ] && [ "$(status_of CHECKA2)" = 0 ]
check "  and this kernel agrees: no /b.txt, /a.txt intact" $?
halt_vm
fsimg_on "$DISK" fsck >/dev/null 2>&1
check "  halted, clean" $?

# --- C: killed at random in a storm -----------------------------------
storm() {                       # storm IMG SEED -> leaves $WORK/fix.* behind
    boot "$1"
    send "/jtest churn /c$2 2000 $2"
    wait_for 'CHURN (1[0-9][0-9]|[2-9][0-9][0-9]|[0-9]{4})' 1800
    sleep "0.$((RANDOM % 10))$((RANDOM % 10))"
    stop
    fsck_fix "$1"
}
bad=0; journal_used=0
for seed in 11 22 33 44 55; do
    newdisk "$WORK/storm.img"
    storm "$WORK/storm.img" $seed
    only_journal_fixed && fixed_clean || { bad=$((bad + 1)); cp "$WORK/fix.out" "$WORK/storm-$seed.fix"; }
    grep -q 'recovering journal' "$WORK/fix.out" && journal_used=$((journal_used + 1))
done
DISK="$SCRATCH/hd-journal.img"
[ $bad -eq 0 ]
check "killed five times mid-storm: e2fsck never needs anything but the journal ($bad needed more)" $?
cp "$WORK/storm.img" "$WORK/storm-last.img"
boot "$WORK/storm.img"
run MAN "/fsmanifest / > /tmp/m"
halt_vm
fsimg_on "$WORK/storm.img" fsck >/dev/null 2>&1
check "  the last one booted, replayed by this kernel, walked, halted: clean" $?
if [ $SUDO = 1 ]; then
    linux_replays "$WORK/storm-last.img" "" ""
    check "  and Linux mounts the killed volume, replays it, and leaves it clean" $?
else
    skipped "Linux mounting a killed volume (needs sudo)"
fi

# --- E: the journal itself damaged -----------------------------------
# jblock IMG N: the volume block holding journal block N.
jblock() { dbg "bmap <8> $2" "$1?offset=$OFF" | tr -dc 0-9; }
# zap IMG VOLBLOCK BYTES: overwrite the start of a volume block.
zap() { printf '%s' "$3" | dd of="$1" bs=1 seek=$((OFF + $2 * 4096)) conv=notrunc status=none; }
committed_then() {              # committed_then IMG FILE: stopped after the commit block
    newdisk "$1"
    boot "$1"
    send "/jtest sync; /jtest stop 257 1; /jtest write $2 50; /jtest sync"
    wait_for 'journal: stopped after the commit block' 300
    stop
}

committed_then "$WORK/E1.img" /e1.txt
zap "$WORK/E1.img" "$(jblock "$WORK/E1.img" 0)" 'XXXX'
DISK="$WORK/E1.img"
boot "$WORK/E1.img"
said 'the journal was damaged and has been made afresh'
check "a committed transaction, then its journal's superblock destroyed: the kernel makes a new one" $?
said 'not cleanly unmounted; checked'
check "  and checks the volume, since what the journal held is gone" $?
run E1C "/jtest check /e1.txt 50"
[ "$(status_of E1C)" != 0 ]
check "  the lost change is absent, not half there" $?
halt_vm
fsimg_on "$WORK/E1.img" fsck >/dev/null 2>&1 &&
    dbg logdump "$WORK/E1.img?offset=$OFF" | grep -q 'Journal starts at block 0'
check "  halted: e2fsck finds it clean, journal and all" $?

committed_then "$WORK/E2.img" /e2.txt
# The commit block: the first log block whose header says type 2.
cb=""
for n in $(seq 1 40); do
    b=$(jblock "$WORK/E2.img" $n)
    t=$(dd if="$WORK/E2.img" bs=1 skip=$((OFF + b * 4096 + 4)) count=4 status=none | od -An -tx1 | tr -d ' \n')
    [ "$t" = 00000002 ] && { cb=$b; break; }
done
[ -n "$cb" ] && zap "$WORK/E2.img" "$cb" 'XXXXXXXXXXXX'
fsck_fix "$WORK/E2.img"
[ -n "$cb" ] && only_journal_fixed && fixed_clean &&
    ! dbg "stat /e2.txt" "$WORK/fixed.part" | grep -q 'Inode:'
check "its commit block destroyed: e2fsck discards the transaction, nothing else to fix" $?
DISK="$WORK/E2.img"
boot "$WORK/E2.img"
run E2C "/jtest check /e2.txt 50"
run E2S "/jtest stats"
[ "$(status_of E2C)" != 0 ] && said 'replayed=0$'
check "  and so does this kernel: nothing replayed, the file absent" $?
halt_vm
fsimg_on "$WORK/E2.img" fsck >/dev/null 2>&1
check "  halted, clean" $?
DISK="$SCRATCH/hd-journal.img"

# --- D: the control ---------------------------------------------------
damaged=0
for seed in 66 77 88; do
    newdisk "$WORK/plain.img" nojournal
    storm "$WORK/plain.img" $seed
    only_journal_fixed && fixed_clean || damaged=$((damaged + 1))
done
DISK="$SCRATCH/hd-journal.img"
[ $damaged -ge 1 ]
check "control: the same storm WITHOUT a journal leaves damage e2fsck finds ($damaged of 3)" $?

grep -aqE 'panic|bus error|address error|DOUBLE MMU FAULT' "$LOG"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed, $skip skipped ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
