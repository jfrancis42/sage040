#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# wgettest.sh - GNU Wget on the machine, against the same host servers
# curltest.sh uses (kernel/websrv.py), reached at 10.0.2.2.
#
# As in curltest, every download is checked by the machine's own
# SHA-256 of what wget wrote against the host's sha256sum of what the
# server holds, and every "wget asked for X" claim is checked in the
# SERVER's log, not in wget's own output.
#
#   http         by address, and by name through /etc/hosts; 2 MB file
#   redirect     a 302 is followed (wget's default)
#   gzip         --compression=gzip: gzip on the wire, original on disk
#   post         --post-file: the server hashes what arrived
#   resume       -c on a truncated file asks for the REST (Range header)
#   recursive    -r -l 1 fetches the page and what it links to, and --
#                the control -- NOT what the linked page links to
#   https        --ca-certificate with the test CA; the default store
#                refuses it (exit 5), --no-check-certificate gets
#                through, and a certificate for the wrong name is
#                refused (exit 5)
#   idn          http://bücher.test/ with LANG=C.UTF-8 arrives as
#                xn--bcher-kva.test
#   psl          of two cookies, the one for all of co.uk is refused
#   no resolv    a .invalid name with no /etc/resolv.conf: exit 4
#                ("network failure"), not a crash
#   internet     https://curl.se against the Mozilla bundle, when the
#                host is online; SKIPPED, never passed, otherwise
set -u
cd "$(dirname "$0")"
. ../machine.conf
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
S=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$S"
DISK="$S/hd-wget.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "wgettest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/wget.log"; FIFO="$S/wget.fifo"; WORK="$S/wget.tmp"
rm -f "$LOG" "$FIFO"; rm -rf "$WORK"; mkdir -p "$WORK/www"
HTTP_PORT=${HTTP_PORT:-8095}
HTTPS_PORT=${HTTPS_PORT:-8445}

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SSLOUT=$SRCDIR/build-openssl-sage040/sage040

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh ifconfig || exit 1
make -s -C ../ldso || exit 1
../ports/wget/build.sh > "$WORK/build.log" 2>&1 || {
    tail -20 "$WORK/build.log"; echo "wgettest: ports/wget failed" >&2; exit 1; }

for p in $HTTP_PORT $HTTPS_PORT; do
    if ss -lnt 2>/dev/null | grep -q ":$p "; then
        echo "wgettest: something is already listening on port $p" >&2; exit 1
    fi
done

echo "=== test data and certificates ==="
python3 - "$WORK/www" <<'PY'
import sys
d = sys.argv[1]
open(d + "/small.txt", "w").write("hello from the host\n" * 50)
b = bytearray()
while len(b) < 2 * 1024 * 1024:
    b += b"SuckOS wget test %08d " % len(b)
    b += bytes(range(256))
    b += b"\x00" * 300
open(d + "/big.bin", "wb").write(bytes(b[:2 * 1024 * 1024]))
open(d + "/part.bin", "wb").write(bytes(b[:100000]))
open(d + "/post.bin", "wb").write(bytes((i * 31 + 7) & 0xFF for i in range(50000)))
# Two levels of links: -l 1 must take index.html's, and not page2's.
open(d + "/index.html", "w").write(
    '<html><body><a href="small.txt">s</a> <a href="page2.html">p</a></body></html>\n')
open(d + "/page2.html", "w").write(
    '<html><body><a href="deep.txt">too deep</a></body></html>\n')
open(d + "/deep.txt", "w").write("two levels down\n")
PY
sha(){ sha256sum "$1" | cut -d' ' -f1; }
SMALL=$(sha "$WORK/www/small.txt"); BIG=$(sha "$WORK/www/big.bin")
POST=$(sha "$WORK/www/post.bin"); PAGE2=$(sha "$WORK/www/page2.html")

( cd "$WORK" &&
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=wgettest-ca \
      -keyout ca.key -out ca.pem &&
  openssl req -newkey rsa:2048 -nodes -subj /CN=testhost \
      -keyout srv.key -out srv.csr &&
  printf 'subjectAltName=DNS:testhost\n' > srv.ext &&
  openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
      -days 3650 -extfile srv.ext -out srv.pem ) > "$WORK/certs.log" 2>&1 || {
    cat "$WORK/certs.log"; echo "wgettest: making certificates failed" >&2; exit 1; }

python3 websrv.py "$WORK/www" "$HTTP_PORT" "$HTTPS_PORT" \
    "$WORK/srv.pem" "$WORK/srv.key" "$WORK/requests.log" 0 > "$WORK/srv.out" 2>&1 &
srv_pid=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/small.txt" && break; sleep 0.1
done

ONLINE=0
curl -s -o /dev/null --max-time 10 https://curl.se/ && ONLINE=1

echo "=== preparing $DISK ==="
rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=48 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /etc; fsimg mkdir /ST
fsimg put -m 755 ../system/sh /bin/sh
fsimg put -m 755 ../system/ifconfig /bin/ifconfig
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 "$SSLOUT/bin/openssl" /bin/openssl
printf '127.0.0.1 localhost\n10.0.2.2 testhost\n10.0.2.2 xn--bcher-kva.test\n10.0.2.2 www.example.co.uk\n' > "$WORK/hosts"
fsimg put "$WORK/hosts" /etc/hosts
fsimg put "$WORK/ca.pem" /ST/ca.pem
fsimg put "$WORK/www/post.bin" /ST/post.bin
# The first 100000 bytes of big.bin, under its name: what -c continues.
fsimg put "$WORK/www/part.bin" /ST/big.bin
# The install under test: the port's own target, which brings ca-certs.
make -s -C ../ports/wget install DISK="$DISK" > "$WORK/install.log" 2>&1 || {
    cat "$WORK/install.log"; echo "wgettest: make install failed" >&2
    kill $srv_pid; exit 1; }

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
  -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
  -nic user,id=n0 \
  -chardev stdio,id=con,signal=off -serial chardev:con \
  < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"
finish(){ exec 3>&- 2>/dev/null; kill $pid $srv_pid 2>/dev/null
          wait $pid $srv_pid 2>/dev/null; }

seen(){ [ "$(tr -d '\r' < "$LOG" | grep -cxF -- "$1")" -ge 1 ]; }
wait_for(){
    for _ in $(seq 1 "${2:-900}"); do
        seen "$1" && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run(){                          # run COMMAND TAG [TIMEOUT/0.2s]
    printf 'echo ==%s\r' "$2" >&3
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2" "${3:-900}" || echo "    (timed out waiting for $2)"
}

for _ in $(seq 1 60); do
    printf 'echo UP\r' >&3
    wait_for UP 10 && break
done
seen UP || { echo "wgettest: the machine never answered"; finish; exit 1; }

H=http://testhost:$HTTP_PORT
IP=http://10.0.2.2:$HTTP_PORT
TS=https://testhost:$HTTPS_PORT
# -q: quiet; -t 1: one try, so a failure is a failure and not twenty
W='wget -q -t 1'
echo "=== running on the machine ==="
run 'ifconfig dhcp' dhcp
run 'cd /ST' cd
run 'export LANG=C.UTF-8' lang
run 'wget --version' version
run "$W http://nosuch.invalid/; echo rc=\$?" invalid 1800
run "$W -O small.txt $IP/small.txt; echo rc=\$?" small
run 'openssl dgst -sha256 -r small.txt' small-sum
run "$W -O whole.bin $H/big.bin; echo rc=\$?" big 1800
run 'openssl dgst -sha256 -r whole.bin' big-sum
run "$W -O redir.txt $H/redir; echo rc=\$?" redir
run 'openssl dgst -sha256 -r redir.txt' redir-sum
run "$W --compression=gzip -O gz.bin $H/gz/big.bin; echo rc=\$?" gz 1800
run 'openssl dgst -sha256 -r gz.bin' gz-sum
run "$W -O - --post-file=post.bin $H/post" post
run "$W -c $H/big.bin; echo rc=\$?" resume 1800
run 'openssl dgst -sha256 -r big.bin' resume-sum
run "$W -r -l 1 -nH -P rec $H/index.html; echo rc=\$?" rec 1800
run 'openssl dgst -sha256 -r rec/small.txt rec/page2.html' rec-sum
run "$W --ca-certificate=ca.pem -O tls.txt $TS/small.txt; echo rc=\$?" tls 1800
run 'openssl dgst -sha256 -r tls.txt' tls-sum
run "$W -O /dev/null $TS/small.txt; echo rc=\$?" tls-default 1800
run "$W --no-check-certificate -O /dev/null $TS/small.txt; echo rc=\$?" tls-k 1800
run "$W --ca-certificate=ca.pem -O /dev/null https://10.0.2.2:$HTTPS_PORT/small.txt; echo rc=\$?" tls-name 1800
run "$W -O idn.txt http://bücher.test:$HTTP_PORT/small.txt; echo rc=\$?" idn 1800
run 'openssl dgst -sha256 -r idn.txt' idn-sum
run "$W --save-cookies jar.txt --keep-session-cookies -O /dev/null http://www.example.co.uk:$HTTP_PORT/cookie; echo rc=\$?" psl-set 1800
run "$W --load-cookies jar.txt -O - http://www.example.co.uk:$HTTP_PORT/echo-cookie" psl-get 1800
if [ "$ONLINE" = 1 ]; then
    run "$W -O /dev/null https://curl.se/; echo rc=\$?" internet 3000
fi
run 'echo ==END' end
finish

CLEAN="$S/wget-clean.txt"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' > "$CLEAN"

echo "=== guest session ==="
sed -n '/^==version/,$p' "$CLEAN" | grep -v '^echo ' | sed 's/^/  | /'

out(){ sed -n "/^==$1\$/,/^DONE-$1\$/p" "$CLEAN" | sed '1d;$d'; }
rc(){ out "$1" | grep -oE '^rc=[0-9]+' | tail -1 | cut -d= -f2; }

pass=0; fail=0; skip=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }

echo "=== checks ==="
out version | grep -q '^GNU Wget 1\.25\.0'
check "wget --version names GNU Wget 1.25.0" $?
for f in +ssl/openssl +psl +iri +zlib +https; do
    out version | grep -q -- "$f" || { false; break; }
done
check "  built with +ssl/openssl +psl +iri +zlib +https" $?

[ "$(rc invalid)" = 4 ]
check "a DNS lookup with no resolv.conf is a network failure (exit 4), not a crash" $?

[ "$(rc small)" = 0 ] && out small-sum | grep -qF "$SMALL"
check "http by address: the small file arrives intact" $?
[ "$(rc big)" = 0 ] && out big-sum | grep -qF "$BIG"
check "http by NAME (/etc/hosts): the 2 MB file arrives intact" $?
[ "$(rc redir)" = 0 ] && out redir-sum | grep -qF "$SMALL"
check "a 302 is followed to the right file" $?

[ "$(rc gz)" = 0 ] && out gz-sum | grep -qF "$BIG"
check "--compression=gzip: the original bytes on disk" $?
grep -q 'GET /gz/big.bin .*ae=.*gzip' "$WORK/requests.log"
check "  and wget asked for gzip (the server's log)" $?

out post | grep -qxF "$POST"
check "a 50 KB --post-file reaches the server intact (the server's hash)" $?

[ "$(rc resume)" = 0 ] && out resume-sum | grep -qF "$BIG"
check "-c completes a truncated file correctly" $?
grep -q 'GET /big.bin range=bytes=100000-' "$WORK/requests.log"
check "  by asking for the rest, not the whole file again" $?

[ "$(rc rec)" = 0 ] && out rec-sum | grep -qF "$SMALL" && out rec-sum | grep -qF "$PAGE2"
check "-r -l 1 fetches the page and both of its links" $?
! grep -q 'GET /deep.txt' "$WORK/requests.log" && grep -q 'GET /page2.html' "$WORK/requests.log"
check "control: and NOT the link two levels down (the server's log)" $?

[ "$(rc tls)" = 0 ] && out tls-sum | grep -qF "$SMALL"
check "https with --ca-certificate: verified, and the file arrives intact" $?
[ "$(rc tls-default)" = 5 ]
check "control: the test CA is NOT in the default store (exit 5)" $?
[ "$(rc tls-k)" = 0 ]
check "control: --no-check-certificate gets through, so that was verification" $?
[ "$(rc tls-name)" = 5 ]
check "control: a certificate for the wrong NAME is refused (exit 5)" $?

[ "$(rc idn)" = 0 ] && out idn-sum | grep -qF "$SMALL"
check "IDN: http://bücher.test/ fetched with LANG=C.UTF-8" $?
grep -q "GET /small.txt .*host=xn--bcher-kva.test:$HTTP_PORT" "$WORK/requests.log"
check "  and it arrived as xn--bcher-kva.test (the server's Host header)" $?

out psl-get | grep -q 'site=1'
check "PSL: a cookie for example.co.uk is kept" $?
out psl-get | grep -q 'cookie=' && ! out psl-get | grep -q 'wide=1'
check "PSL: a cookie for all of co.uk is REFUSED" $?

if [ "$ONLINE" = 1 ]; then
    [ "$(rc internet)" = 0 ]
    check "https://curl.se verifies against the Mozilla bundle" $?
else
    echo "  [SKIP] https://curl.se -- the host is offline"; skip=$((skip+1))
fi

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ "$skip" -gt 0 ] && echo "  skipped: $skip"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
