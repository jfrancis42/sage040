#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# devtest.sh - the devices on interrupts, the NVRAM, and the limits
# (task 22).
#
#   The serial port, the keyboard and the disk each counted taking
#   interrupts -- the keyboard typed at through the monitor's sendkey, as
#   there is no window to type in; the disk's waits sleeping, not
#   polling. Six processes in the filesystem at once, with every disk
#   request slowed (a test knob) so that they really are asleep inside
#   it while the others work -- every byte checked, and the volume
#   checked afterwards with the host's e2fsck.
#   Every static limit filled and then passed. Settings written to the
#   NVRAM, the machine reset -- this QEMU runs WITHOUT -no-reboot, so a
#   reset really restarts it -- and the settings read back, and an
#   interface configured from them. Both ways of stopping it are
#   exercised: `reboot`, which asks for the reset the 8042 can actually
#   do, and `shutdown`, which asks to go away and has to settle for the
#   same reset because nothing on this board can cut the supply. That
#   they are different requests reaching the same line is the point --
#   the console says so, and this checks that it does.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"

. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-dev.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/devtest.log"
# Gone before QEMU starts, so a run that never reaches the guest has no
# log to grade -- rather than silently grading the last run's.
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

MON="$SCRATCH/devtest.mon"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../apps || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin
for p in irqs nvram ifconfig shutdown; do
    fsimg put -m 755 ../system/$p "/bin/$p"
done
fsimg put -m 755 ../apps/fsstress /fsstress
fsimg put -m 755 ../apps/limits /limits
fsimg put -m 755 ../apps/fbmap /fbmap

rm -f "$SCRATCH/in.fifo" "$MON"
mkfifo "$SCRATCH/in.fifo"
# No -no-reboot: `shutdown` resets the machine and it boots again, which
# is what the NVRAM check needs. The harness ends it by killing QEMU.
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none \
    -monitor "unix:$MON,server,nowait" \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 600); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2"
    sleep 0.2
}
# Keys through the monitor: the 8042, not the serial port.
type_keys() {
    local k
    for k in "$@"; do
        echo "sendkey $k" | socat - "unix:$MON" >/dev/null 2>&1
        sleep 0.05
    done
}

run 'irqs' irq0
# "echo kbd-ok" on the keyboard, then its own marker the same way.
type_keys e c h o spc k b d minus o k ret
sleep 1
run 'irqs' irq1
run '/fsstress 15' stress
run 'irqs' irq2
run '/limits' limits
# The console off the screen first: its own output scrolls the whole
# screen up, and ten lines of fbmap's results carry the boxes off the top.
run 'console fbcon off' conser
run '/fbmap' fbmap
# What reached the screen, not what the program believes it wrote.
printf 'screendump %s\n' "$SCRATCH/fbmap.ppm" | socat - "unix:$MON" >/dev/null
sleep 1
run 'console fbcon on' conboth
run 'irqs' irq3
run 'nvram net.ip=10.9.8.7' nv1
run 'nvram net.mask=255.255.255.0' nv2
run 'nvram greeting=hello-nvram' nv3
run 'nvram greeting' nv4
run 'nvram -d greeting' nv5
run 'nvram greeting; echo NV6=$?' nv6
run 'nvram' nvlist
# The reset, with `reboot` -- the command that asks for exactly what
# this board can do. The boot banner is the sign it happened.
reset_with() {
    n0=$(grep -c "kernel ready" "$LOG")
    printf '%s\r' "$1" >&3
    for _ in $(seq 1 300); do
        [ "$(grep -c "kernel ready" "$LOG")" -gt "$n0" ] && break
        kill -0 "$qemu_pid" 2>/dev/null || break
        sleep 0.2
    done
    sleep "$BOOT_WAIT"
}
reset_with reboot
run 'nvram net.ip' after
run 'ifconfig nvram' ifc
# And `shutdown`, which wants the power cut, cannot have it, and says
# what it is doing instead of pretending.
reset_with shutdown
run 'nvram net.ip' after2
# Not run(): a halted machine never prints the DONE marker.
printf 'shutdown -h\r' >&3
wait_for "halting."
sleep 1

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo" "$MON"
tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed -n '/kernel ready/,$p' "$SCRATCH/clean.tmp" | sed 's/^/  | /'

between() {
    awk -v s="$1" -v e="DONE-$2" '
        $0 == e { printf "%s", buf; exit }
        index($0, "$ " s) || $0 == s { buf = ""; f = 1; next }
        f { buf = buf $0 "\n" }' "$SCRATCH/clean.tmp"
}
count_of() {                   # count_of NAME MARKER
    between irqs "$2" | awk -v n="$1" '$3 == n { print $2; exit }'
}

echo "=== checks: interrupts ==="
s0=$(count_of serial irq0); s1=$(count_of serial irq1)
[ "${s0:-0}" -gt 0 ] && [ "${s1:-0}" -gt "${s0:-0}" ]
check "the serial port takes interrupts ($s0, then $s1)" $?
grep -qx "kbd-ok" "$SCRATCH/clean.tmp"
check "a command typed on the keyboard ran" $?
k1=$(count_of keyboard irq1)
[ "${k1:-0}" -gt 0 ]
check "  and the keyboard took interrupts to do it ($k1)" $?
d1=$(count_of disk irq1); d2=$(count_of disk irq2)
[ "${d2:-0}" -gt "${d1:-0}" ]
check "the disk takes interrupts ($d1, then $d2)" $?
slept=$(between irqs irq2 | sed -n 's/^  disk waits: \([0-9]*\) slept.*/\1/p')
[ "${slept:-0}" -gt 0 ]
check "  and a program waiting for it sleeps ($slept waits)" $?
between irqs irq2 | grep -q 'spurious 0, input overruns 0'
check "  with no interrupt nobody asked for, and no input lost" $?

echo "=== checks: the filesystem, four processes at once ==="
between '/fsstress' stress | grep -qx "fsstress: every byte of every file intact"
check "three writers and three churners in one directory, each disk request slowed to 15 ms: every byte intact" $?

echo "=== checks: the limits ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(between '/limits' limits | grep -E '^  (ok  |FAIL) ')
between '/limits' limits | grep -qx "limits: 0 failed"
check "limits ran to the end" $?

echo "=== checks: /dev/fb0 as memory ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(between '/fbmap' fbmap | grep -E '^  (ok  |FAIL) ')
between '/fbmap' fbmap | grep -qx "fbmap: 0 failed"
check "fbmap ran to the end" $?
python3 - "$SCRATCH/fbmap.ppm" > "$SCRATCH/fbpix.tmp" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
parts = d.split(maxsplit=4)
w, h = int(parts[1]), int(parts[2])
px = parts[4]
def at(x, y):
    i = (y * w + x) * 3
    return tuple(px[i:i + 3])
print("red" if at(10, 10) == (255, 0, 0) else "notred", at(10, 10))
print("green" if at(310, 10) == (0, 255, 0) else "notgreen", at(310, 10))
print("black" if at(250, 50) == (0, 0, 0) else "notblack", at(250, 50))
PY
sed 's/^/  | /' "$SCRATCH/fbpix.tmp"
grep -q '^red ' "$SCRATCH/fbpix.tmp"
check "the screen shows red where the program wrote red through the mapping" $?
grep -q '^green ' "$SCRATCH/fbpix.tmp"
check "  and green where its forked child did" $?
grep -q '^black ' "$SCRATCH/fbpix.tmp"
check "  and nothing between them" $?
rm -f "$SCRATCH/fbmap.ppm" "$SCRATCH/fbpix.tmp"

echo "=== checks: the kernel stack ==="
kline=$(between irqs irq3 | grep 'kernel stack:')
echo "  $kline"
kmax=$(echo "$kline" | sed -n 's/.*stack: \([0-9]*\) of \([0-9]*\) bytes.*/\1/p')
ksize=$(echo "$kline" | sed -n 's/.*stack: \([0-9]*\) of \([0-9]*\) bytes.*/\2/p')
[ "${kmax:-0}" -gt 512 ] && [ "${ksize:-0}" -gt 0 ] && [ "$kmax" -lt $((ksize * 3 / 4)) ]
check "after all of the above, no kernel stack past three quarters full ($kmax of $ksize bytes)" $?

echo "=== checks: the NVRAM ==="
between 'nvram greeting' nv4 | grep -qx "hello-nvram"
check "a setting written and read back" $?
between 'nvram greeting' nv6 | grep -qx "NV6=1"
check "  deleted, and then not there (status 1)" $?
between nvram nvlist | grep -qx "net.ip=10.9.8.7" &&
    between nvram nvlist | grep -qx "net.mask=255.255.255.0"
check "  listed" $?
[ "$(grep -c "kernel ready" "$SCRATCH/clean.tmp")" -ge 2 ]
check "reboot reset the machine and it booted again" $?
grep -qx "restarting." "$SCRATCH/clean.tmp"
check "  and said so before it went" $?
between 'nvram net.ip' after | grep -qx "10.9.8.7"
check "  and the setting survived it" $?

# The distinction between the two requests is the thing being checked:
# RB_AUTOBOOT gets the reset it asked for and says nothing about power,
# RB_POWER_OFF cannot have what it asked for and says so. If this ever
# starts failing because the message moved, move it here too -- do not
# delete the check, because without it `reboot` and `shutdown` being
# one function with two names would pass just as well.
[ "$(grep -c "kernel ready" "$SCRATCH/clean.tmp")" -ge 3 ]
check "shutdown reset it too -- this board has no other way to stop" $?
grep -q "no power control on this board; resetting" "$SCRATCH/clean.tmp"
check "  and said that a reset is not the power cut it asked for" $?
! between 'reboot' after | grep -q "no power control"
check "  while reboot, which asked for the reset, said no such thing" $?
between 'nvram net.ip' after2 | grep -qx "10.9.8.7"
check "  and the setting survived that reset as well" $?
between 'ifconfig nvram' ifc | grep -q "inet 10.9.8.7  netmask 255.255.255.0"
check "ifconfig nvram configured the interface from it" $?

echo "=== checks: the volume, on the host ==="
# No dd: e2fsprogs reaches into the partition by offset, so the
# filesystem is checked where it lives.
fsimg fsck > "$SCRATCH/devfsck.tmp" 2>&1
fsck_rc=$?
sed 's/^/  | /' "$SCRATCH/devfsck.tmp"
[ "$fsck_rc" -eq 0 ]
check "e2fsck finds nothing wrong after all of that" $?
rm -f "$SCRATCH/devfsck.tmp"

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
