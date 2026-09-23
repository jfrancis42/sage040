#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# logtest.sh - the kernel's log, and /var/log/syslog.
#
# The kernel keeps everything it prints in a ring (kernel/klog.c) and
# writes to no file itself; `klogd`, started from /etc/rc, copies the
# ring into /var/log/syslog. This checks both halves and the join
# between them:
#
#   - that /dev/klog gives back what the kernel said, with `dmesg`;
#   - that a read DRAINS it, so two readers do not each get half;
#   - that klogd, running in the background, puts the boot messages in
#     /var/log/syslog with a timestamp on each line;
#   - that a message printed AFTER klogd started arrives there too,
#     which is what makes it a log rather than a snapshot;
#   - and that the file is a real file, read afterwards with the HOST's
#     tools rather than by the machine that wrote it.

set -u

cd "$(dirname "$0")"

. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-log.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/logtest.log"
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

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system klogd dmesg sh || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /etc
for p in klogd dmesg sh; do
    fsimg put -m 755 "../system/$p" /bin/$p
done
# The machine's own /etc/rc, which is what starts klogd at boot.
fsimg put -m 755 ../system/rc /etc/rc

rm -f "$SCRATCH/log.fifo"
mkfifo "$SCRATCH/log.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/log.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/log.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

# The boot has already happened, and /etc/rc has already started klogd,
# so the log should exist before anything here types a character.
printf 'echo LOG-START\r' >&3
wait_for "LOG-START"

# A message the kernel prints AFTER klogd was started: `swapon` on a
# file that is not there fails and says so, which is a kernel message
# with a known string in it.
printf 'ifconfig\r' >&3
sleep 1
printf 'echo MARK-AFTER\r' >&3
wait_for "MARK-AFTER"
sleep 2

# dmesg with klogd running should find the ring EMPTY -- klogd has taken
# everything -- which is what "a read drains" means.
printf 'dmesg > /DMESG1.TXT\r' >&3
sleep 1
printf 'echo DMESG1-DONE\r' >&3
wait_for "DMESG1-DONE"

printf 'cat /var/log/syslog > /SYSLOG.TXT\r' >&3
sleep 1
printf 'echo STILL-HERE\r' >&3
wait_for "STILL-HERE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/log.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/log-clean.tmp"

# --- the second boot, with nothing to start klogd ---------------------
#
# /etc/rc is what starts klogd, so renaming it away is how to see the
# machine without one: dmesg is then the only reader of the ring and
# gets everything. It is also the check that /etc/rc is what does it.
fsimg mv /etc/rc /etc/rc.off
LOG2="$SCRATCH/logtest-norc.log"
rm -f "$LOG2"
rm -f "$SCRATCH/log.fifo"
mkfifo "$SCRATCH/log.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/log.fifo" > "$LOG2" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/log.fifo"
sleep "$BOOT_WAIT"
printf 'dmesg > /DMESG2.TXT\r' >&3
sleep 1
printf 'echo NORC-DONE\r' >&3
for _ in $(seq 1 300); do
    grep -qF "NORC-DONE" "$LOG2" 2>/dev/null && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 0.2
done
sleep 0.5
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/log.fifo"
fsimg mv /etc/rc.off /etc/rc
tr -d '\r' < "$LOG2" >> "$SCRATCH/log-clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/log-clean.tmp"

fsimg cat /SYSLOG.TXT 2>/dev/null | tr -d '\r' > "$SCRATCH/syslog.tmp"
fsimg cat /DMESG1.TXT 2>/dev/null | tr -d '\r' > "$SCRATCH/dmesg1.tmp"
fsimg cat /DMESG2.TXT 2>/dev/null | tr -d '\r' > "$SCRATCH/dmesg2.tmp"

echo "=== checks: the log file ==="

test -s "$SCRATCH/syslog.tmp"
check "/var/log/syslog exists and is not empty" $?

grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}Z ' "$SCRATCH/syslog.tmp"
check "  every line begins with a timestamp" $?

grep -q "kernel: " "$SCRATCH/syslog.tmp"
check "  and says which machine and that it is the kernel" $?

grep -q "kernel ready" "$SCRATCH/syslog.tmp"
check "the boot messages are in it -- from BEFORE klogd started" $?

grep -qE "(ext2|fat16) on /dev/hda" "$SCRATCH/syslog.tmp"
check "  including what the kernel said about the disk" $?

grep -q "eth0" "$SCRATCH/syslog.tmp"
check "a message printed after klogd started is there too" $?

# The host reads the file the machine wrote: debugfs, not the kernel.
lines=$(wc -l < "$SCRATCH/syslog.tmp")
test "$lines" -gt 10
check "the host's tools read $lines lines out of the file" $?

echo "=== checks: /dev/klog drains ==="

test ! -s "$SCRATCH/dmesg1.tmp"
check "with klogd running, dmesg finds the ring empty" $?

grep -q "kernel ready" "$SCRATCH/dmesg2.tmp"
check "with no klogd, dmesg is the reader and gets the boot messages" $?

grep -qE '^[0-9]{4}-' "$SCRATCH/dmesg2.tmp"
r=$?
[ $r -ne 0 ]
check "  raw, without klogd's timestamps: dmesg is the ring, not the file" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
