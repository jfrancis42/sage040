#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# dnstest.sh - names, and the time.
#
# The resolver in lib/ulib, and ntpdate, against servers this script runs
# on the host (netservers.py) and the guest reaches through QEMU's
# user-mode network at 10.0.2.2. Nothing here needs the internet, and
# every answer is known in advance:
#
#   an A record; a CNAME chain two deep; a name in another case; a
#   forged reply with the wrong ID, sent ahead of the real one;
#   NXDOMAIN; a name the server never answers, to see the timeout;
#   /etc/hosts; localhost; a dotted quad; ping and ntpdate by name;
#   and SNTP from a server whose clock says 2031.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"

. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-dns.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
MIMG="$DISK@@$OFFSET"
LOG="$SCRATCH/dnstest.log"
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

DNS_PORT=${DNS_PORT:-15353}
NTP_PORT=${NTP_PORT:-11123}
NTP_TIME=1939291200            # 2031-06-15 12:00:00 UTC
NTP_LATE=2214109800            # 2040-02-29 06:30:00 UTC, past NTP's wrap

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../apps || exit 1
make -s -C ../libc/test inettest || {
    echo "dnstest.sh: could not build inettest -- is picolibc built (make libc)?" >&2
    exit 1
}

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (16 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mmd -i "$MIMG" ::/BIN ::/ETC
for p in ifconfig ping host ntpdate; do
    mcopy -o -i "$MIMG" ../system/$p ::/BIN/$(echo $p | tr a-z A-Z)
done
mcopy -o -i "$MIMG" ../libc/test/inettest ::/INETTEST

if command -v ss >/dev/null 2>&1 &&
   ss -lnu 2>/dev/null | grep -qE ":($DNS_PORT|$NTP_PORT|$((NTP_PORT + 1))) "; then
    echo "dnstest: port $DNS_PORT or $NTP_PORT is already in use" >&2
    exit 1
fi
python3 -u ./netservers.py "$DNS_PORT" "$NTP_PORT" "$NTP_TIME" "$NTP_LATE" \
    > "$SCRATCH/netservers.log" 2>&1 &
srv_pid=$!
sleep 0.5

rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot -nic user \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 300); do
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

run 'ifconfig dhcp' dhcp
run 'ifconfig' ifc
run "echo 'nameserver 10.0.2.2#$DNS_PORT' > /etc/resolv.conf" conf
run "echo '10.4.5.6 myhost myalias' > /etc/hosts" hosts
run 'host foo.sage.test' a
run 'host chain.sage.test' cname
run 'host upper.SAGE.test' case
run 'host nosuch.sage.test' nx
run 'echo HOST-RC=$?' nxrc
run 'host myalias' hostsfile
run 'host localhost' local
run 'host 10.20.30.40' quad
run 'host silent.sage.test' silent
run 'host spoof.sage.test' spoof
run 'ping gw.sage.test 2' ping
run '/INETTEST' inet
run "ntpdate -q -p $NTP_PORT ntp.sage.test" ntpq
run 'date' date1
run "ntpdate -p $NTP_PORT ntp.sage.test" ntp
run 'date' date2
run "ntpdate -p $((NTP_PORT + 1)) ntp.sage.test" ntplate
run 'date' date3
run "echo 'nameserver 10.0.2.2#1' > /etc/resolv.conf" dead
run 'host foo.sage.test' deadserver

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
kill "$srv_pid" 2>/dev/null
wait "$srv_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo"
tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed -n '/kernel ready/,$p' "$SCRATCH/clean.tmp" | sed 's/^/  | /'
echo "=== the host's servers ==="
sed 's/^/  | /' "$SCRATCH/netservers.log"

# The output between a command and its DONE marker.
between() {
    awk -v s="$1" -v e="DONE-$2" '
        index($0, s) && !f { f = 1; next }
        f && index($0, e) { exit }
        f' "$SCRATCH/clean.tmp"
}

echo "=== checks: names ==="
between 'ifconfig' ifc | grep -q "dns 10.0.2.3"
check "ifconfig shows the name server DHCP handed out" $?
between 'host foo.sage.test' a | grep -qx "foo.sage.test has address 10.1.2.3"
check "an A record" $?
between 'host chain.sage.test' cname | grep -qx "chain.sage.test has address 10.1.2.3"
check "through two CNAMEs to the address" $?
between 'host upper.SAGE.test' case | grep -qx "upper.SAGE.test has address 10.9.9.9"
check "a name asked in another case" $?
between 'host nosuch.sage.test' nx | grep -q "not found" &&
    between 'echo HOST-RC' nxrc | grep -qx "HOST-RC=1"
check "NXDOMAIN is not found, exit status 1" $?
between 'host myalias' hostsfile | grep -qx "myalias has address 10.4.5.6"
check "/etc/hosts, by an alias" $?
between 'host localhost' local | grep -qx "localhost has address 127.0.0.1"
check "localhost" $?
between 'host 10.20.30.40' quad | grep -qx "10.20.30.40 has address 10.20.30.40"
check "a dotted quad is its own answer" $?
between 'host silent.sage.test' silent | grep -q "no answer"
check "a server that never answers times out" $?
test "$(grep -c 'dns query for silent.sage.test ' "$SCRATCH/netservers.log")" -eq 2
check "  after asking it twice, as the resolver is meant to" $?
between 'host spoof.sage.test' spoof | grep -qx "spoof.sage.test has address 10.7.7.7"
check "a reply with the wrong ID is ignored, not believed" $?
test "$(between 'ping gw.sage.test' ping | grep -c 'reply from 10.0.2.2')" -eq 2
check "ping by name" $?
between 'host foo.sage.test' deadserver | grep -q "no answer"
check "a name server nobody runs gives no answer, not a wrong one" $?

echo "=== checks: picolibc's network layer (inettest) ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(between '/INETTEST' inet | grep -E '^  (ok  |FAIL) ')
between '/INETTEST' inet | grep -qx "inettest: 0 failed"
check "inettest ran to the end" $?
# host asks once; inettest's getaddrinfo once, and its lookup in capitals
# and its gethostbyname come from its cache.
test "$(grep -ci 'dns query for foo.sage.test ' "$SCRATCH/netservers.log")" -eq 2
check "  and the server was asked for foo.sage.test twice: host, then inettest once" $?

echo "=== checks: the time ==="
between "ntpdate -q" ntpq | grep -q "ntpdate: ntp.sage.test stratum 2, offset +"
check "ntpdate -q reports the offset from the server's clock" $?
! between 'date' date1 | grep -q "2031"
check "  and does not set the clock" $?
between "ntpdate -p" ntp | grep -qx "ntpdate: clock set"
check "ntpdate sets it" $?
between 'date' date2 | grep -q "15 June 2031"
check "  to the server's time, which date then shows" $?
between 'date' date3 | grep -q "29 February 2040"
check "a server's time past NTP's 2036 wrap is 2040, not 1904" $?

rm -f "$SCRATCH/netservers.log"

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
