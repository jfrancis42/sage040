#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# uptimetest.sh - the load average actually measures load, over time.
#
# uptime's easy half is the clock and the user count; its real claim is
# the load average, and a load average is the kind of thing that passes
# every cheap test while being wrong -- it can print a plausible number
# that never moves, or that only ever climbs, or that counts the idle
# task and so sits near 1.00 on a machine doing nothing. So this drives
# the machine through three states and checks the number against each:
#
#   idle      right after boot, nothing running: load must be LOW. This
#             is the negative control for "the idle task is not counted"
#             -- if it were, an idle machine would read ~1.00.
#   loaded    four busy spinners for a while: load must CLIMB past 1.00,
#             toward the number of runnable tasks.
#   cooling   the spinners killed: load must FALL again. A cumulative
#             counter would keep rising here; a moving average decays,
#             and that is the difference this checks.
#
# It waits real seconds because the averages are defined over real
# minutes -- there is no shortcut, the same way there is none on a real
# machine. Runs on a rescue-shell image (no /bin/login), so it comes up
# straight to a root shell.
set -u
cd "$(dirname "$0")"
. ../machine.conf
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
S=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$S"
DISK="$S/hd-uptime.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "uptimetest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/uptime.log"; FIFO="$S/uptime.fifo"
rm -f "$LOG" "$FIFO"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh uptime || exit 1
make -s -C ../apps spin || exit 1

rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin
fsimg put -m 755 ../system/sh     /bin/sh
fsimg put -m 755 ../system/uptime /bin/uptime
fsimg put -m 755 ../apps/spin      /bin/spin

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
  -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
  -chardev stdio,id=con,signal=off -serial chardev:con \
  < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"
sleep 6
send(){ printf '%s\r' "$1" >&3; sleep "${2:-1}"; }

# IDLE. One period after boot the first sample has landed; read it.
send 'echo ==IDLE'
send '/bin/uptime' 2

# LOADED. Four spinners, backgrounded; the shell announces each as
# "[PID]  spin &" -- the bracketed number IS the pid, which is how they
# get killed later.
send 'spin &' 1
send 'spin &' 1
send 'spin &' 1
send 'spin &' 1
echo "  (four spinners running; waiting for the 1-min average to climb)"
sleep 45
send 'echo ==LOADED'
send '/bin/uptime' 2

# COOLING. Kill every pid the shell announced, then let it decay.
# De-escape first: the raw log carries CSI and bracketed-paste markers
# (which also start with '['), so a bare ^\[ anchor matches only the
# lines that happen not to be prefixed by one.
PIDS=$(tr -d '\r' < "$LOG" \
    | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' \
    | grep -E '\[[0-9]+\][[:space:]]+spin' | grep -oE '[0-9]+' | sort -un)
echo "  spinner pids: $PIDS"
for p in $PIDS; do send "kill $p" 1; done
echo "  (spinners killed; waiting for the average to fall)"
sleep 45
send 'echo ==COOLING'
send '/bin/uptime' 2

# The two extra forms, cheap and worth covering.
send 'echo ==PRETTY'; send '/bin/uptime -p' 2
send 'echo ==SINCE';  send '/bin/uptime -s' 2
send 'echo ==DONE' 2
for i in $(seq 1 30); do grep -q '==DONE' "$LOG" && break; sleep 1; done
sleep 2
exec 3>&-; kill $pid 2>/dev/null; wait $pid 2>/dev/null
CLEAN="$S/uptime-clean.txt"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' > "$CLEAN"

echo "=== guest session ==="
sed -n '/==IDLE/,$p' "$CLEAN" | sed 's/^/  | /'

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }

# The first load number on the uptime line after a marker.
load_after(){ sed -n "/==$1/,\$p" "$CLEAN" | grep -m1 'load average:' \
    | sed 's/.*load average: *//' | cut -d, -f1; }
# awk float compare: `flt A OP B` -> exit 0 if true
flt(){ awk -v a="$1" -v b="$3" "BEGIN{exit !(a $2 b)}"; }

L0=$(load_after IDLE); L1=$(load_after LOADED); L2=$(load_after COOLING)
echo "=== readings: idle=$L0  loaded=$L1  cooling=$L2 ==="

echo "=== checks ==="
grep -q 'load average:' "$CLEAN"
check "uptime prints a load average at all" $?
sed -n '/==IDLE/,$p' "$CLEAN" | grep -qE 'up .*(min|:)'
check "  and how long the machine has been up" $?

[ -n "$L0" ] && [ -n "$L1" ] && [ -n "$L2" ]
check "three readings were captured" $?

flt "$L0" "<" 1.0
check "idle: load is below 1.00 -- the idle task is NOT counted" $?

flt "$L1" ">=" 1.0
check "loaded: four spinners drive the 1-min load past 1.00" $?
flt "$L1" ">" "$(awk -v x="$L0" 'BEGIN{print x+0.8}')"
check "  and it climbed well above the idle reading" $?
flt "$L1" "<" 6.0
check "  while staying near the number of busy tasks, not runaway" $?

flt "$L2" "<" "$L1"
check "cooling: the load FELL once the work stopped -- an average," $?
flt "$L2" "<" "$(awk -v x="$L1" 'BEGIN{print x-0.3}')"
check "  not a counter that only ever rises" $?

sed -n '/==PRETTY/,/==SINCE/p' "$CLEAN" | grep -qE 'up .*(minute|hour|day)'
check "uptime -p prints the duration in words" $?
sed -n '/==SINCE/,/==DONE/p' "$CLEAN" | grep -qE '20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]'
check "uptime -s prints when it came up" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
