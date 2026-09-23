#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# lotest.sh - the loopback interface, and the wire's side of 127/8.
#
# `lo` is an interface like eth0: 127.0.0.1/8, its own counters, and it
# can be taken down. What goes to 127/8 -- or to this machine's own
# address -- travels on lo and never on the wire.
#
# And the other direction: a frame arriving from the WIRE addressed to,
# or claiming to come from, 127/8 is a martian and is dropped. Without
# that, anything on the LAN can reach a service bound to 127.0.0.1 by
# addressing a frame to this machine's MAC, which is the whole reason
# for binding a service to 127.0.0.1.
#
# The wire here is QEMU's `-nic socket,udp=...`: every frame the guest
# sends arrives on a host UDP port, and anything sent to another port
# arrives at the guest as though off the cable. wire.py is the host's
# end of it. A test of "dropped" is only worth anything next to a frame
# of the same shape that is NOT dropped, so each martian is paired with
# an ordinary datagram sent the same way.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-lo.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/lotest.log"
WIRE="$SCRATCH/lowire.hex"
rm -f "$LOG" "$WIRE"
BOOT_WAIT=${BOOT_WAIT:-4}
# The guest's side is RX (frames to it), the host listens on TX.
RXPORT=${LO_RXPORT:-17000}
TXPORT=${LO_TXPORT:-17001}
GUEST=10.9.9.2

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
make -s -C ../apps || exit 1
make -s -C ../system || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg put -m 755 ../apps/udpwait /udpwait
fsimg mkdir /bin
for p in ifconfig ping; do
    fsimg put -m 755 "../system/$p" "/bin/$p"
done

for port in "$RXPORT" "$TXPORT"; do
    if ss -lnu 2>/dev/null | grep -q "127.0.0.1:$port "; then
        echo "lotest: UDP port $port is in use; set LO_RXPORT/LO_TXPORT" >&2
        exit 1
    fi
done

python3 wire.py capture "$TXPORT" "$WIRE" &
cap_pid=$!

rm -f "$SCRATCH/lo.fifo"
mkfifo "$SCRATCH/lo.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -nic socket,id=n0,udp=127.0.0.1:$TXPORT,localaddr=127.0.0.1:$RXPORT \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/lo.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/lo.fifo"

wait_for() {                    # wait_for TEXT [COUNT]
    local want=${2:-1}
    for _ in $(seq 1 300); do
        [ "$(tr -d '\r' < "$LOG" | grep -cF -- "$1")" -ge "$want" ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2" 2
    sleep 0.2
}
# udpwait in the foreground, and a frame put on the wire once it is
# listening. N counts the listeners so far, so each waits for its own.
nlisten=0
inject() {                      # inject TAG BINDADDR PORT SRC DST
    nlisten=$((nlisten + 1))
    printf '/udpwait %s %s 3\r' "$2" "$3" >&3
    printf 'echo DONE-%s\r' "$1" >&3
    wait_for "udpwait: listening" "$nlisten"
    python3 wire.py send "$RXPORT" "$MAC" "$4" "$5" "$3" "HELLO-$1"
    wait_for "DONE-$1" 2
    sleep 0.2
}

sleep "$BOOT_WAIT"
run "ifconfig $GUEST 255.255.255.0 10.9.9.1" addr
MAC=$(tr -d '\r' < "$LOG" | sed -n 's/^eth0  hwaddr \([0-9a-f:]*\).*/\1/p' | head -1)
run 'ifconfig lo' lo0
run 'ping 127.0.0.1 3' ping1
run 'ifconfig lo' lo1
run "ping $GUEST 2" pingself
run 'ifconfig eth0' eth1
run 'ifconfig lo' lo2
run 'ifconfig lo down; echo DOWNRC=$?' down
run 'ping 127.0.0.1 1; echo PINGRC=$?' pingdown
run 'ifconfig lo' lo3
run 'ifconfig lo up' up
run 'ping 127.0.0.1 1; echo PINGRC=$?' pingup
run 'ifconfig nosuch; echo NOSUCH=$?' nosuch
run 'ifconfig' all
# The pairs: the ordinary datagram first, then its martian twin.
inject wire     0.0.0.0   7001 10.9.9.1  "$GUEST"
inject todst    127.0.0.1 7002 10.9.9.1  127.0.0.1
inject tolo2    0.0.0.0   7003 10.9.9.1  127.0.0.2
inject fromlo   0.0.0.0   7004 127.0.0.1 "$GUEST"
run 'ifconfig eth0' eth2
# Something that MUST reach the wire -- an ARP request for a neighbour
# -- so that "nothing with 127/8 on the wire" is known to be looking.
run 'ping 10.9.9.1 1' pingwire

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
kill "$cap_pid" 2>/dev/null
wait "$cap_pid" 2>/dev/null
rm -f "$SCRATCH/lo.fifo"
tr -d '\r' < "$LOG" > "$SCRATCH/lo.tmp"

echo "=== guest session ==="
sed -n '/kernel ready/,$p' "$SCRATCH/lo.tmp" | sed 's/^/  | /'

between() {
    awk -v s="$1" -v e="DONE-$2" '
        $0 == e { printf "%s", buf; exit }
        index($0, "$ " s) || $0 == s { buf = ""; f = 1; next }
        f { buf = buf $0 "\n" }' "$SCRATCH/lo.tmp"
}
field() {                       # field TAG IFACE WHICH: RX or TX packets
    between "ifconfig $2" "$1" |
        sed -n "s/.*$3 \([0-9]*\) packets.*/\1/p" | head -1
}

echo "=== checks: the interface ==="
[ -n "$MAC" ]
check "the guest's MAC is known ($MAC)" $?
between 'ifconfig lo' lo0 | grep -q '^lo  .*UP.*LOOPBACK'
check "lo exists, up, and says it is a loopback" $?
between 'ifconfig lo' lo0 | grep -q 'inet 127\.0\.0\.1  netmask 255\.0\.0\.0'
check "  127.0.0.1, netmask 255.0.0.0" $?
between 'ifconfig' all | grep -q '^eth0 ' &&
    between 'ifconfig' all | grep -q '^lo '
check "ifconfig with no arguments lists both" $?
between 'ifconfig nosuch; echo NOSUCH=$?' nosuch | grep -q 'NOSUCH=1'
check "an interface that does not exist is an error" $?

echo "=== checks: traffic to 127/8 and to this machine stays off the wire ==="
test "$(between 'ping 127.0.0.1 3' ping1 | grep -c 'reply from 127.0.0.1')" -eq 3
check "ping 127.0.0.1: three replies" $?
r0=$(field lo0 lo RX); t0=$(field lo0 lo TX)
r1=$(field lo1 lo RX); t1=$(field lo1 lo TX)
[ $((r1 - r0)) -eq 6 ] && [ $((t1 - t0)) -eq 6 ]
check "  counted on lo: 6 in and 6 out ($r0->$r1, $t0->$t1)" $?
test "$(between "ping $GUEST 2" pingself | grep -c "reply from $GUEST")" -eq 2
check "ping of this machine's own address" $?
r2=$(field lo2 lo RX)
[ $((r2 - r1)) -eq 4 ]
check "  went over lo too ($r1->$r2)" $?
e1=$(field eth1 eth0 TX); e2=$(field eth2 eth0 TX)
[ "${e1:-x}" = 0 ]
check "  and eth0 sent nothing for any of it (TX $e1)" $?
read -r wtotal wmartian < <(python3 wire.py summary "$WIRE")
[ "$wtotal" -gt 0 ]
check "the host's end of the wire sees what the guest sends ($wtotal frames)" $?
[ "$wmartian" -eq 0 ]
check "no frame on the wire carried a 127/8 address ($wtotal frames, $wmartian)" $?

echo "=== checks: lo taken down ==="
between 'ifconfig lo down; echo DOWNRC=$?' down | grep -q 'DOWNRC=0'
check "ifconfig lo down" $?
between 'ifconfig lo' lo3 | grep -q '^lo  .*DOWN'
check "  and it says so" $?
between 'ping 127.0.0.1 1; echo PINGRC=$?' pingdown | grep -q 'network is unreachable'
check "  127.0.0.1 is then unreachable, and ping says why" $?
between 'ping 127.0.0.1 1; echo PINGRC=$?' pingdown | grep -q 'PINGRC=1'
check "  with a failing status" $?
between 'ping 127.0.0.1 1; echo PINGRC=$?' pingup | grep -q 'reply from 127.0.0.1'
check "ifconfig lo up brings it back" $?

echo "=== checks: 127/8 arriving from the wire ==="
between '/udpwait 0.0.0.0 7001' wire | grep -q "from 10.9.9.1: HELLO-wire"
check "an ordinary datagram put on the wire arrives (the control)" $?
between '/udpwait 127.0.0.1 7002' todst | grep -q 'udpwait: nothing'
check "  the same to 127.0.0.1 is dropped, even with a listener bound there" $?
between '/udpwait 0.0.0.0 7003' tolo2 | grep -q 'udpwait: nothing'
check "  to 127.0.0.2, anywhere in 127/8, dropped" $?
between '/udpwait 0.0.0.0 7004' fromlo | grep -q 'udpwait: nothing'
check "  FROM 127.0.0.1 to this machine, dropped" $?
[ "${e2:-0}" -eq 0 ]
check "  and none of it was answered on the wire (eth0 TX $e2)" $?

rm -f "$SCRATCH/lo.tmp"
echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
else
    echo "RESULT: FAIL"
fi
