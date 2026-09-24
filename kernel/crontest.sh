#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# crontest.sh - cron: something the machine does by itself, later.
#
# Everything else on this machine happens because somebody typed it.
# cron is the first thing that happens because the CLOCK said so, which
# makes this as much a test of the clock, of a long-lived background
# task, and of syslog as of sbase's crond.
#
# THE TEST HAS TO WAIT FOR REAL TIME. A minute-resolution cron cannot be
# hurried, so the suite sets the machine's clock to a few seconds before
# a minute boundary and waits for it to cross -- twice, because one job
# firing could be a coincidence of startup and two cannot.
#
# The negative control is a crontab entry for a minute that will NOT
# come round during the run: it must not fire, or the matching is not
# matching anything.

set -u

cd "$(dirname "$0")"

. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-cron.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/crontest.log"
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
make -s -C ../system klogd sh || exit 1
if [ ! -x ../ports/sbase/bin/cron ]; then
    make -s -C ../ports/sbase || exit 1
fi

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /etc; fsimg mkdir /lib; fsimg mkdir /var; fsimg mkdir /var/log; fsimg mkdir /var/run
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
for p in cron echo date cat; do
    [ -x "../ports/sbase/bin/$p" ] && \
        fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p
done
fsimg put -m 755 ../system/klogd /bin/klogd
fsimg put -m 755 ../system/sh /bin/sh

# The crontab: one job every minute, and one at a minute that will not
# come round while this runs (the control).
#
# min hour mday mon wday command
cat > "$SCRATCH/crontab.tmp" <<'EOF'
* * * * * /bin/echo cron-fired >> /CRON.OUT
30 4 1 1 * /bin/echo control-fired >> /CRON.OUT
EOF
fsimg put "$SCRATCH/crontab.tmp" /etc/crontab

rm -f "$SCRATCH/cron.fifo"
mkfifo "$SCRATCH/cron.fifo"

"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/cron.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/cron.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

# Ten seconds before a minute boundary, so the first firing is soon and
# the wait below is bounded by something known rather than by luck.
printf 'date -s 2026-03-04 12:00:50\r' >&3
sleep 1
printf 'date\r' >&3
sleep 1
printf 'cron\r' >&3
sleep 2
printf 'echo CRON-STARTED\r' >&3
wait_for "CRON-STARTED"

# Two minute boundaries: 12:01 and 12:02. The guest's clock runs at
# real time, so this is a real wait of about two minutes.
echo "    waiting for two minute boundaries on the machine's clock..."
sleep 150

printf 'cat /CRON.OUT\r' >&3
sleep 2
printf 'cat /var/log/syslog > /SYSLOG.TXT\r' >&3
sleep 2
printf 'date\r' >&3
sleep 1
printf 'echo CRON-DONE\r' >&3
wait_for "CRON-DONE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/cron.fifo"
tr -d '\r' < "$LOG" > "$SCRATCH/cron-clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/cron-clean.tmp" | tail -40

fsimg cat /CRON.OUT 2>/dev/null | tr -d '\r' > "$SCRATCH/cronout.tmp"
fsimg cat /SYSLOG.TXT 2>/dev/null | tr -d '\r' > "$SCRATCH/cronsyslog.tmp"

echo "=== checks ==="

grep -q "CRON-STARTED" "$SCRATCH/cron-clean.tmp"
check "cron started and the shell carried on" $?

fired=$(grep -c '^cron-fired$' "$SCRATCH/cronout.tmp" 2>/dev/null || echo 0)
test "$fired" -ge 2
check "the every-minute job ran at least twice ($fired times)" $?

test "$fired" -le 4
check "  and not more often than the minutes that passed" $?

! grep -q "control-fired" "$SCRATCH/cronout.tmp"
check "the job for a minute that did not come round did NOT run" $?

# The job's output is a file the HOST reads, not something the machine
# printed: cron ran a program that wrote to the disk.
test -s "$SCRATCH/cronout.tmp"
check "the job's output is on the disk, read back with the host's own tools" $?

grep -qi "cron" "$SCRATCH/cronsyslog.tmp"
check "cron said what it was doing in /var/log/syslog" $?

! grep -qE "panic|exception at|DOUBLE MMU" "$SCRATCH/cron-clean.tmp"
check "no panic, no kernel exception" $?

grep -q "CRON-DONE" "$SCRATCH/cron-clean.tmp"
check "the machine is still there afterwards" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
