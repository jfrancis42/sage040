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

#
# Everything this test writes goes in one place.
#
# Scratch disk images are 16 MB each and there is one per test suite, so
# leaving them beside the source meant 67 MB of build product scattered
# through the tree with names that looked like part of it. They are all
# under scratch/ now, which `make clean` removes and git ignores.
#
#
# Computed AFTER the cd above, from the working directory rather than
# from $0 -- which has already been used once and is relative to where
# the script was invoked from, not to where it now is. Deriving it from
# $0 a second time worked when the script was run as ./edittest.sh and
# failed when it was run by path, which is a difference nobody should
# have to notice.
#
SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-net.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/nettest.log"
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
make -s -C ../apps || exit 1
make -s -C ../system || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mcopy -o -i "$MIMG" ../apps/fetch ::/FETCH
#
# The network tools are programs now, not shell builtins, so the test
# has to install them the way a real disk would -- in /bin, which is
# where PATH looks first.
#
mmd -i "$MIMG" ::/BIN 2>/dev/null || true
for p in ifconfig ping netstat; do
    mcopy -o -i "$MIMG" "../system/$p" "::/BIN/$(echo $p | tr a-z A-Z)"
done

#
# A web server on the host. The guest reaches it at 10.0.2.2, which is
# what slirp calls the machine QEMU is running on.
#
rm -rf "$SCRATCH/webroot.tmp"
mkdir -p "$SCRATCH/webroot.tmp"
echo "SMALL-FILE-OK" > "$SCRATCH/webroot.tmp"/small.txt
# Comfortably more than the 2 KB receive buffer, so the window has to
# open and close during the transfer.
i=0
: > "$SCRATCH/webroot.tmp"/big.txt
while [ $i -lt 200 ]; do
    echo "line $i ........................................" >> "$SCRATCH/webroot.tmp"/big.txt
    i=$((i + 1))
done
BIGSIZE=$(wc -c < "$SCRATCH/webroot.tmp"/big.txt)

#
# Refuse to run against a server this test did not start.
#
# A leftover server from an earlier run holds the port, python declines
# to bind, and the guest is then served somebody else's directory -- so
# the TCP works perfectly and every content check fails with a 404. That
# reads as a broken stack and is not one, so it is caught here instead.
#
if command -v ss >/dev/null 2>&1 && ss -lnt 2>/dev/null | grep -q ":$HTTP_PORT "; then
    echo "nettest: something is already listening on port $HTTP_PORT." >&2
    echo "nettest: set HTTP_PORT to something else, or stop it." >&2
    exit 1
fi

#
# `exec`, so that python REPLACES the subshell and $! is python's own
# pid. Without it the kill at the end takes the subshell and leaves the
# server running -- which then holds the port and fails the next run.
#
( cd "$SCRATCH/webroot.tmp" && exec python3 -m http.server "$HTTP_PORT" \
    --bind 127.0.0.1 >/dev/null 2>&1 ) &
http_pid=$!

# Wait for it to actually answer, rather than assuming a second is enough.
ready=0
for _ in $(seq 1 30); do
    if python3 - "$HTTP_PORT" <<'EOF' 2>/dev/null
import socket, sys
s = socket.socket()
s.settimeout(0.5)
try:
    s.connect(("127.0.0.1", int(sys.argv[1])))
    sys.exit(0)
except Exception:
    sys.exit(1)
EOF
    then
        ready=1
        break
    fi
    sleep 0.2
done
if [ "$ready" -ne 1 ]; then
    echo "nettest: the host web server never came up" >&2
    kill "$http_pid" 2>/dev/null
    exit 1
fi

: > "$SCRATCH/session.tmp"
{
    printf 'ifconfig dhcp\r';              sleep 8
    printf 'ifconfig\r';                   sleep 3
    printf 'ping 10.0.2.2 3\r';            sleep 8
    printf 'netstat -a\r';                 sleep 3
    printf "fetch 10.0.2.2 $HTTP_PORT /small.txt\r"; sleep 10
    printf "fetch 10.0.2.2 $HTTP_PORT /big.txt\r";   sleep 20
    printf 'ifconfig\r';                   sleep 2
    printf 'echo NETTEST-DONE\r'
} >> "$SCRATCH/session.tmp"

rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"

# -nic user explicitly: this test must not depend on what the host is
# plugged into.
"$QEMU" -M sage040 -cpu m68040 -m 4 \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot -nic user \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!

exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"
cat "$SCRATCH/session.tmp" >&3

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
rm -f "$SCRATCH/in.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed 's/^/  | /' "$SCRATCH/clean.tmp"

echo "=== checks: the interface ==="

grep -q "net     : eth0 up" "$SCRATCH/clean.tmp"
check "the driver brought the interface up at boot" $?

echo "=== checks: DHCP ==="

grep -q "inet 10.0.2.15" "$SCRATCH/clean.tmp"
check "DHCP got an address, netmask and gateway" $?

echo "=== checks: ARP ==="

grep -qE "^10\.0\.2\.2  at [0-9a-f:]{17}" "$SCRATCH/clean.tmp"
check "ARP resolved the gateway, and netstat showed the cache" $?

echo "=== checks: ICMP ==="

test "$(grep -c 'reply from 10.0.2.2' "$SCRATCH/clean.tmp")" -eq 3
check "all three pings were answered" $?

grep -q "3 sent, 3 received, 0% loss" "$SCRATCH/clean.tmp"
check "  with no loss" $?

echo "=== checks: TCP ==="

grep -qx "SMALL-FILE-OK" "$SCRATCH/clean.tmp"
check "TCP connected and fetched a file over HTTP" $?

grep -q "line 199 " "$SCRATCH/clean.tmp"
check "a transfer larger than the receive buffer completed" $?

# The byte count fetch prints is the whole reply, headers included, so it
# is bigger than the file -- but it must be at least the file's size.
awk -v want="$BIGSIZE" '
    /--- [0-9]+ bytes ---/ { gsub(/[^0-9]/, "", $2); if ($2 + 0 >= want) ok = 1 }
    END { exit ok ? 0 : 1 }
' "$SCRATCH/clean.tmp"
check "  and every byte of it arrived" $?

grep -q "line 0 " "$SCRATCH/clean.tmp"
check "  in order, from the first line" $?

echo "=== checks: the tools are programs ==="

# They run unprivileged, in address spaces of their own. If any of them
# had still been a builtin this would be indistinguishable -- which is
# why the check is that the PROGRAM was found and run from /bin.
grep -q "eth0  hwaddr" "$SCRATCH/clean.tmp"
check "ifconfig ran as a program out of /bin" $?

echo "=== checks: nothing broke ==="

grep -q "NETTEST-DONE" "$SCRATCH/clean.tmp"
check "the shell survived all of it" $?

! grep -q "exception" "$SCRATCH/clean.tmp"
check "no faults anywhere" $?

awk '/TX [0-9]+ packets, [0-9]+ errors/ {
        gsub(/[^0-9]/, "", $NF); if ($NF + 0 != 0) bad = 1
     } END { exit bad ? 1 : 0 }' "$SCRATCH/clean.tmp"
check "no transmit errors" $?

rm -rf "$SCRATCH/webroot.tmp"

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
