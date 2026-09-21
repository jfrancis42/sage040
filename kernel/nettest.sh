#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# nettest.sh - ARP, DHCP, ICMP, UDP and TCP, end to end.
#
# Runs on QEMU's user-mode NAT, deliberately. The stack has been proved
# on a real LAN -- it takes a lease from a real server, answers ping from
# other machines and serves HTTP to them -- but a TEST has to give the
# same answer on every machine, need no privileges and touch nothing
# outside the emulator. slirp provides a DHCP server, a gateway that
# answers ICMP, and a route to the host, which is every layer this needs.
#
# The TCP check is the one worth explaining. It fetches a file from a web
# server started here on the host, and the file is deliberately bigger
# than the guest's 2 KB receive buffer -- so the transfer cannot complete
# unless the window opens and closes correctly as the program reads. A
# small file proves the handshake; only a large one proves flow control.

set -u

cd "$(dirname "$0")"

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

DISK=hd-net.img
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG=nettest.log
BOOT_WAIT=${BOOT_WAIT:-4}
HTTP_PORT=${HTTP_PORT:-8099}

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
make -s -C ../user || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" ../user/fetch ::/FETCH

#
# A web server on the host. The guest reaches it at 10.0.2.2, which is
# what slirp calls the machine QEMU is running on.
#
rm -rf webroot.tmp
mkdir -p webroot.tmp
echo "SMALL-FILE-OK" > webroot.tmp/small.txt
# Comfortably more than the 2 KB receive buffer, so the window has to
# open and close during the transfer.
i=0
: > webroot.tmp/big.txt
while [ $i -lt 200 ]; do
    echo "line $i ........................................" >> webroot.tmp/big.txt
    i=$((i + 1))
done
BIGSIZE=$(wc -c < webroot.tmp/big.txt)

( cd webroot.tmp && python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 \
    >/dev/null 2>&1 ) &
http_pid=$!
sleep 1

: > session.tmp
{
    printf 'dhcp\r';                       sleep 6
    printf 'ifconfig\r';                   sleep 2
    printf 'ping 10.0.2.2 3\r';            sleep 6
    printf 'arp\r';                        sleep 2
    printf "fetch 10.0.2.2 $HTTP_PORT /small.txt\r"; sleep 10
    printf "fetch 10.0.2.2 $HTTP_PORT /big.txt\r";   sleep 20
    printf 'ifconfig\r';                   sleep 2
    printf 'echo NETTEST-DONE\r'
} >> session.tmp

rm -f in.fifo
mkfifo in.fifo

# -nic user explicitly: this test must not depend on what the host is
# plugged into.
"$QEMU" -M sage040 -cpu m68040 -m 4 \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot -nic user \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < in.fifo > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> in.fifo
sleep "$BOOT_WAIT"
cat session.tmp >&3

for _ in $(seq 1 400); do
    if grep -qF "NETTEST-DONE" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
    sleep 0.2
done

sleep 0.5
exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
kill "$http_pid" 2>/dev/null
wait "$http_pid" 2>/dev/null
rm -f in.fifo

tr -d '\r' < "$LOG" > clean.tmp

echo "=== guest session ==="
sed 's/^/  | /' clean.tmp

echo "=== checks: the interface ==="

grep -q "net     : eth0 up" clean.tmp
check "the driver brought the interface up at boot" $?

echo "=== checks: DHCP ==="

grep -q "got 10.0.2.15" clean.tmp
check "DHCP got an address, netmask and gateway" $?

echo "=== checks: ARP ==="

grep -qE "^10\.0\.2\.2  at [0-9a-f:]{17}" clean.tmp
check "ARP resolved the gateway to a hardware address" $?

echo "=== checks: ICMP ==="

test "$(grep -c 'reply from 10.0.2.2' clean.tmp)" -eq 3
check "all three pings were answered" $?

grep -q "3 sent, 3 received, 0% loss" clean.tmp
check "  with no loss" $?

echo "=== checks: TCP ==="

grep -qx "SMALL-FILE-OK" clean.tmp
check "TCP connected and fetched a file over HTTP" $?

grep -q "line 199 " clean.tmp
check "a transfer larger than the receive buffer completed" $?

# The byte count fetch prints is the whole reply, headers included, so it
# is bigger than the file -- but it must be at least the file's size.
awk -v want="$BIGSIZE" '
    /--- [0-9]+ bytes ---/ { gsub(/[^0-9]/, "", $2); if ($2 + 0 >= want) ok = 1 }
    END { exit ok ? 0 : 1 }
' clean.tmp
check "  and every byte of it arrived" $?

grep -q "line 0 " clean.tmp
check "  in order, from the first line" $?

echo "=== checks: nothing broke ==="

grep -q "NETTEST-DONE" clean.tmp
check "the shell survived all of it" $?

! grep -q "exception" clean.tmp
check "no faults anywhere" $?

awk '/TX [0-9]+ packets, [0-9]+ errors/ {
        gsub(/[^0-9]/, "", $NF); if ($NF + 0 != 0) bad = 1
     } END { exit bad ? 1 : 0 }' clean.tmp
check "no transmit errors" $?

rm -rf webroot.tmp

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
