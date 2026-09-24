#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# routetest.sh - the routing table and the ARP cache, seen and changed.
#
# The stack used to route with an if statement (on my subnet? no: the
# gateway) and hold the ARP cache with no way to drop an entry. Both are
# real now: a routing table you can read and add to, and an ARP cache
# you can delete from. This proves the parts that a plausible-looking
# but wrong implementation would get wrong:
#
#   - DHCP's results appear AS ROUTES: an on-link route for the subnet
#     (no gateway) and a default route THROUGH the gateway. If those did
#     not show up, the table would be a decoration that governs nothing.
#   - a route added by hand appears, with the right flags, and can be
#     deleted again; a duplicate is refused; deleting one that is not
#     there is refused. The refusals matter as much as the successes --
#     a table that silently accepts nonsense is worse than none.
#   - `arp -d` forgets an entry, and the resolver RE-LEARNS it on the
#     next packet. That round trip is the whole point of being able to
#     delete one: a host whose MAC changed is reachable again without a
#     reboot.
#
# Runs on slirp (user-mode NAT), like nettest: the machine is
# 10.0.2.15/24, the gateway 10.0.2.2. Deliberately not the real LAN.
set -u
cd "$(dirname "$0")"
. ../machine.conf
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-route.img"
OFFSET=$((2048 * 512))
fsimg(){ PART_OFFSET=$OFFSET ../tools/fsimg.sh "$DISK" "$@" || {
    echo "routetest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/route.log"; FIFO="$SCRATCH/route.fifo"
rm -f "$LOG" "$FIFO"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh ifconfig ping route arp || exit 1

rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin
for p in sh ifconfig ping route arp; do
    fsimg put -m 755 "../system/$p" "/bin/$p"
done

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
  -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
  -nic user,id=n0 \
  -chardev stdio,id=con,signal=off -serial chardev:con \
  < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"
sleep 6
send(){ printf '%s\r' "$1" >&3; sleep "${2:-1}"; }

send 'ifconfig dhcp' 10

# The table DHCP produced.
send 'echo ==ROUTES'; send 'route' 2; send 'echo ==END' 1

# Populate the ARP cache for the gateway, then look.
send 'ping 10.0.2.2 2' 5
send 'echo ==ARP1'; send 'arp' 2; send 'echo ==END' 1

# Add a route to another subnet through the gateway.
send 'route add -net 192.168.99.0 netmask 255.255.255.0 gw 10.0.2.2' 1
send 'echo ==ADDED'; send 'route' 2; send 'echo ==END' 1

# A duplicate default must be refused -- DHCP already made one.
send 'echo ==DUP'; send 'route add default gw 10.0.2.2' 2; send 'echo ==END' 1

# Delete the hand-made route; it goes; deleting it again is refused.
send 'route del -net 192.168.99.0 netmask 255.255.255.0' 1
send 'echo ==DELETED'; send 'route' 2; send 'echo ==END' 1
send 'echo ==NOSUCH'; send 'route del -net 10.9.9.0 netmask 255.255.255.0' 2; send 'echo ==END' 1

# Forget the gateway's hardware address; it is gone; the next packet
# re-learns it.
send 'arp -d 10.0.2.2' 1
send 'echo ==ARPDEL'; send 'arp' 2; send 'echo ==END' 1
send 'ping 10.0.2.2 2' 5
send 'echo ==ARP2'; send 'arp' 2; send 'echo ==END' 1
send 'echo ==ARPNOSUCH'; send 'arp -d 10.9.9.9' 2; send 'echo ==END' 1

send 'echo ROUTETEST-DONE' 2
for i in $(seq 1 60); do grep -q 'ROUTETEST-DONE' "$LOG" && break; sleep 1; done
sleep 1
exec 3>&-; kill $pid 2>/dev/null; wait $pid 2>/dev/null
CLEAN="$SCRATCH/route-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' > "$CLEAN"

echo "=== guest session ==="
sed -n '/==ROUTES/,$p' "$CLEAN" | sed 's/^/  | /'

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }
# The lines of one marked-off block.
block(){ sed -n "/==$1\$/,/==END\$/p" "$CLEAN"; }

echo "=== checks ==="

# --- the table DHCP built ---
block ROUTES | grep -qE '^default .*10\.0\.2\.2 .*UG'
check "the default route goes through the DHCP gateway (flags UG)" $?
block ROUTES | grep -qE '^10\.0\.2\.0 .*255\.255\.255\.0 .*U .*eth0'
check "the subnet is an on-link route, no gateway" $?
block ROUTES | grep -qE '^127\.0\.0\.0 .*255\.0\.0\.0 .*lo'
check "loopback is a route over lo" $?

# --- adding a route ---
block ADDED | grep -qE '^192\.168\.99\.0 .*10\.0\.2\.2 .*255\.255\.255\.0 .*UG'
check "route add put the new subnet in, through the gateway" $?

# --- a duplicate is refused ---
block DUP | grep -qi 'exist'
check "a second default route is refused, not silently doubled" $?

# --- deleting it ---
if block DELETED | grep -qE '192\.168\.99\.0'; then gone=1; else gone=0; fi
check "route del removed the hand-made route" $gone
block DELETED | grep -qE '^default .*10\.0\.2\.2'
check "  and left the interface's own routes alone" $?
block NOSUCH | grep -qi 'no such route'
check "deleting a route that is not there is refused" $?

# --- ARP: delete and re-learn ---
block ARP1 | grep -qi '10\.0\.2\.2'
check "arp shows the gateway after a ping resolved it" $?
if block ARPDEL | grep -qi '10\.0\.2\.2'; then held=1; else held=0; fi
check "arp -d forgot the gateway's hardware address" $held
block ARP2 | grep -qi '10\.0\.2\.2'
check "  and the next ping re-learned it -- the resolver recovers" $?
block ARPNOSUCH | grep -qi 'no such entry'
check "deleting an entry that is not cached is refused" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
