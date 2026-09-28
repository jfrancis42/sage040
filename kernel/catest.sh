#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# catest.sh - the machine trusts the certificate authorities it should,
# found where OpenSSL looks by default, and nothing else.
#
# The claim ports/ca-certs makes is not "a file was copied". It is that
# a program which asks OpenSSL for the DEFAULT trust store -- as curl,
# Python's ssl.create_default_context() and `openssl verify` without
# -CAfile all do -- gets Mozilla's roots. So the checks are:
#
#   installed    the port's own `make install` put the pinned bundle at
#                /etc/ssl/cert.pem, byte for byte, mode 644. Read back
#                with the host's tools, not the machine's.
#   where        the openssl on the machine names /etc/ssl as its
#                directory. If ports/openssl were ever built with another
#                --openssldir, the file would be in the wrong place and
#                every other check here could still be arranged to pass.
#   read         the machine reads the same bytes (its own SHA-256 of the
#                file agrees with the pin) and finds the same number of
#                certificates in it that the host counts.
#   verify       a real chain -- curl.se's, as served on 2026-09-26 --
#                verifies with NO -CAfile. Checked at that date with
#                -attime, so the leaf expiring does not fail the suite.
#   controls     the same chain with every default store switched off
#                FAILS, which proves the success came from cert.pem and
#                not from the chain somehow vouching for itself; and a
#                self-signed certificate made here FAILS against the
#                defaults, which proves the store is not trust-everything.
#
# The chain is ports/ca-certs/test/leaf.pem, and chain.pem holds the two
# intermediates the server sent. Its root, ISRG Root X1, is in the
# bundle; the intermediates are not.
#
# Runs on a rescue-shell image (no /bin/login): sh, the dynamic loader,
# libc.so and openssl, nothing else.
set -u
cd "$(dirname "$0")"
. ../machine.conf
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
S=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$S"
DISK="$S/hd-ca.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "catest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/ca.log"; FIFO="$S/ca.fifo"; WORK="$S/ca.tmp"
rm -f "$LOG" "$FIFO"; rm -rf "$WORK"; mkdir -p "$WORK"

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SSLOUT=$SRCDIR/build-openssl-sage040/sage040
TESTDIR=../ports/ca-certs/test
# 2026-09-26 00:00:00 UTC: inside every certificate's validity in the
# chain, and a day before the host captured it. Fixed, not "now".
AT=1790380800

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../ldso || exit 1
../ports/openssl/build.sh > /dev/null 2>&1 || {
    echo "catest: ports/openssl/build.sh failed" >&2; exit 1; }
[ -x "$SSLOUT/bin/openssl" ] || {
    echo "catest: no openssl command in $SSLOUT/bin" >&2; exit 1; }

PIN=$(sed -n 's/^SHA256=//p' ../ports/ca-certs/build.sh)
PEM=$SRCDIR/ca-certs/cert.pem

# A certificate no bundle contains, made fresh each run.
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=nobody \
    -keyout "$WORK/self.key" -out "$WORK/self.pem" 2>/dev/null || {
    echo "catest: the host could not make a self-signed certificate" >&2
    exit 1; }

echo "=== preparing $DISK ==="
rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /ST
fsimg put -m 755 ../system/sh /bin/sh
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 "$SSLOUT/bin/openssl" /bin/openssl
fsimg put "$TESTDIR/leaf.pem"  /ST/leaf.pem
fsimg put "$TESTDIR/chain.pem" /ST/chain.pem
fsimg put "$WORK/self.pem"     /ST/self.pem
# The install under test: the port's own target, not a re-spelling of it.
make -s -C ../ports/ca-certs install DISK="$DISK" > "$WORK/install.log" 2>&1 || {
    cat "$WORK/install.log"; echo "catest: make install failed" >&2; exit 1; }

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
  -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
  -chardev stdio,id=con,signal=off -serial chardev:con \
  < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"

seen(){ [ "$(tr -d '\r' < "$LOG" | grep -cxF -- "$1")" -ge 1 ]; }
wait_for(){
    for _ in $(seq 1 "${2:-600}"); do
        seen "$1" && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run(){                          # run COMMAND TAG
    printf 'echo ==%s\r' "$2" >&3
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2" || echo "    (timed out waiting for $2)"
}

# Ready when the shell answers. Anything sent before the console is up
# is thrown away, so asking again is harmless.
for _ in $(seq 1 60); do
    printf 'echo UP\r' >&3
    wait_for UP 10 && break
done
seen UP || { echo "catest: the machine never answered"; kill $pid; exit 1; }

run 'cd /ST' cd
run 'openssl version -d' where
run 'openssl dgst -sha256 -r /etc/ssl/cert.pem' digest
run 'openssl storeutl -noout -certs /etc/ssl/cert.pem' count
run "openssl verify -attime $AT -untrusted chain.pem leaf.pem" verify
run "openssl verify -attime $AT -no-CAfile -no-CApath -no-CAstore -untrusted chain.pem leaf.pem" nostore
run 'openssl verify self.pem' self
run 'echo ==END' end

exec 3>&-; kill $pid 2>/dev/null; wait $pid 2>/dev/null
CLEAN="$S/ca-clean.txt"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' > "$CLEAN"

echo "=== guest session ==="
sed -n '/^==where/,$p' "$CLEAN" | sed 's/^/  | /'

# The output of one command: the lines between its marker and its DONE.
out(){ sed -n "/^==$1\$/,/^DONE-$1\$/p" "$CLEAN" | sed '1d;$d'; }

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }

echo "=== checks ==="
[ "$(fsimg cat /etc/ssl/cert.pem | sha256sum | cut -d' ' -f1)" = "$PIN" ]
check "make install put the pinned bundle at /etc/ssl/cert.pem (host reads it back)" $?
fsimg ls-l /etc/ssl | grep -qE '^ *[0-9]+ +100644 .* cert\.pem$'
check "  mode 644" $?

out where | grep -qF 'OPENSSLDIR: "/etc/ssl"'
check "the machine's openssl looks in /etc/ssl" $?

out digest | grep -qF "$PIN"
check "the machine's SHA-256 of cert.pem agrees with the pin" $?

want=$(grep -c 'BEGIN CERTIFICATE' "$PEM")
out count | grep -qxF "Total found: $want"
check "the machine finds $want certificates in it, as the host counts" $?

out verify | grep -qxF 'leaf.pem: OK'
check "curl.se's chain verifies against the DEFAULT store (no -CAfile)" $?

out nostore | grep -qxF 'leaf.pem: OK'
[ $? -ne 0 ] && out nostore | grep -q 'verification failed'
check "control: with the default stores off, the same chain FAILS" $?

out self | grep -qxF 'self.pem: OK'
[ $? -ne 0 ] && out self | grep -q 'verification failed'
check "control: a self-signed certificate is NOT trusted" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
