#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# curltest.sh - curl on the machine, against servers this script runs on
# the host, reached through QEMU's user-mode network at 10.0.2.2.
#
# Every download is checked by the machine's own SHA-256 of what curl
# wrote (openssl dgst) against the host's sha256sum of what the server
# holds -- two implementations, two CPUs. A check that curl "printed
# something" would pass on a truncated or corrupted transfer.
#
#   http         a small file and a 2 MB one (the large one crosses
#                many TCP windows and every one of curl's buffers)
#   names        a host name from /etc/hosts, so the lookup goes
#                through the machine's getaddrinfo, not a literal
#   redirect     -L follows a 302
#   compressed   --compressed with a server that answers in gzip; the
#                result must be the ORIGINAL bytes
#   post         -d @file: the server hashes what it received
#   resume       -C - continues a partial file, and the server is
#                asked for a Range (the server records it)
#   https        a server with a certificate from a test CA made here,
#                verified with --cacert
#   controls     the same server with the DEFAULT trust store must fail
#                (curl exit 60): verification is really happening. With
#                -k it succeeds: so the failure was verification and not
#                the network. And a certificate for the wrong NAME must
#                fail too, which is the check that makes https mean
#                anything.
#   s_client     `openssl s_client` reaches the same TLS server and
#                verifies it. It used to fail before connecting:
#                OpenSSL's BIO_socket_nbio() had no FIONBIO to use
#                (libc/patches/38).
#   no resolv    a name NOT in /etc/hosts, with no /etc/resolv.conf on
#                the disk, so libc asks the kernel for DHCP's name
#                server (NETCTL_INFO). That path overflowed the stack
#                and killed every such lookup with a bus error; nothing
#                caught it because every other suite writes a
#                resolv.conf. The name is .invalid (RFC 6761), so the
#                answer is "no such name" -- curl exit 6 -- online or
#                off, and a crash is 139. Offline, slirp's DNS may not
#                answer at all, which is also exit 6.
#   br, zstd     --compressed against a server that answers in brotli,
#                and one that answers in zstd: the ORIGINAL bytes again
#   http2        a server that speaks ONLY HTTP/2 (TLS, ALPN "h2");
#                curl must negotiate it and report http_version 2.
#                Needs the host's python h2 module (requirements.txt);
#                SKIPPED, never passed, without it
#   idn          http://bücher.test/ with LANG=C.UTF-8 reaches the host
#                named xn--bcher-kva.test in /etc/hosts -- and the server
#                saw that name in the Host header
#   psl          a server on www.example.co.uk sets one cookie for
#                example.co.uk and one for all of co.uk; curl must keep
#                the first and refuse the second
#   tools        idn2, psl and brotli on the machine agree with the
#                host's own idn2, psl and brotli
#   internet     if the host can reach https://curl.se, the machine
#                must too, verified against the Mozilla bundle from
#                ports/ca-certs. SKIPPED, never passed, when offline.
#
# The TLS handshake time is printed (curl's time_appconnect). It is a
# measurement, not a check: curl-lynx.md's open question.
set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
S=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$S"
DISK="$S/hd-curl.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "curltest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/curl.log"; FIFO="$S/curl.fifo"; WORK="$S/curl.tmp"
rm -f "$LOG" "$FIFO"; rm -rf "$WORK"; mkdir -p "$WORK/www"
HTTP_PORT=${HTTP_PORT:-8097}
HTTPS_PORT=${HTTPS_PORT:-8443}
H2_PORT=${H2_PORT:-8444}
HAVE_H2=0
python3 -c 'import h2' 2>/dev/null && HAVE_H2=1

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SSLOUT=$SRCDIR/build-openssl-sage040/sage040
CURLOUT=$SRCDIR/build-curl-sage040/sage040

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh ifconfig || exit 1
make -s -C ../ldso || exit 1
../ports/curl/build.sh > "$WORK/build.log" 2>&1 || {
    tail -20 "$WORK/build.log"; echo "curltest: ports/curl failed" >&2; exit 1; }

for p in $HTTP_PORT $HTTPS_PORT $H2_PORT; do
    if ss -lnt 2>/dev/null | grep -q ":$p "; then
        echo "curltest: something is already listening on port $p" >&2; exit 1
    fi
done

echo "=== test data and certificates ==="
python3 - "$WORK/www" <<'PY'
import sys
d = sys.argv[1]
open(d + "/small.txt", "w").write("hello from the host\n" * 50)
# Deterministic, not random: text, every byte value, long runs.
b = bytearray()
while len(b) < 2 * 1024 * 1024:
    b += b"SuckOS curl test %08d " % len(b)
    b += bytes(range(256))
    b += b"\x00" * 300
open(d + "/big.bin", "wb").write(bytes(b[:2 * 1024 * 1024]))
open(d + "/post.bin", "wb").write(bytes((i * 31 + 7) & 0xFF for i in range(50000)))
PY
sha(){ sha256sum "$1" | cut -d' ' -f1; }
SMALL=$(sha "$WORK/www/small.txt"); BIG=$(sha "$WORK/www/big.bin")
POST=$(sha "$WORK/www/post.bin")
# Compressed copies made by the HOST's own tools, for the server to
# send and for the machine's brotli to decode.
brotli -q 11 -c "$WORK/www/big.bin" > "$WORK/www/big.bin.br"
zstd -q -19 -c "$WORK/www/big.bin" > "$WORK/www/big.bin.zst"
H_IDN=$(idn2 bücher.example 2>/dev/null)
H_PSL=$(psl --print-unreg-domain www.example.co.uk 2>/dev/null)

# A test CA, and a server certificate from it naming "testhost" ONLY --
# no IP address -- so that https://10.0.2.2 is a name mismatch.
( cd "$WORK" &&
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=curltest-ca \
      -keyout ca.key -out ca.pem &&
  openssl req -newkey rsa:2048 -nodes -subj /CN=testhost \
      -keyout srv.key -out srv.csr &&
  printf 'subjectAltName=DNS:testhost\n' > srv.ext &&
  openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
      -days 3650 -extfile srv.ext -out srv.pem ) > "$WORK/certs.log" 2>&1 || {
    cat "$WORK/certs.log"; echo "curltest: making certificates failed" >&2; exit 1; }

# The servers: kernel/websrv.py, shared with wgettest.sh.
python3 websrv.py "$WORK/www" "$HTTP_PORT" "$HTTPS_PORT" \
    "$WORK/srv.pem" "$WORK/srv.key" "$WORK/requests.log" \
    "$([ "$HAVE_H2" = 1 ] && echo "$H2_PORT" || echo 0)" > "$WORK/srv.out" 2>&1 &
srv_pid=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/small.txt" && break; sleep 0.1
done

# Is the internet there? Decided on the HOST, before the guest is asked.
ONLINE=0
curl -s -o /dev/null --max-time 10 https://curl.se/ && ONLINE=1

echo "=== preparing $DISK ==="
rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=48 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /etc; fsimg mkdir /ST
put_shells
fsimg put -m 755 ../system/ifconfig /bin/ifconfig
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 "$SSLOUT/bin/openssl" /bin/openssl
printf '127.0.0.1 localhost\n10.0.2.2 testhost\n10.0.2.2 xn--bcher-kva.test\n10.0.2.2 www.example.co.uk\n' > "$WORK/hosts"
fsimg put "$WORK/hosts" /etc/hosts
fsimg put "$WORK/ca.pem" /ST/ca.pem
fsimg put "$WORK/www/post.bin" /ST/post.bin
fsimg put "$WORK/www/big.bin.br" /ST/host.br
for t in libidn2:idn2 libpsl:psl brotli:brotli; do
    ../ports/${t%%:*}/build.sh > /dev/null 2>&1 || {
        echo "curltest: ports/${t%%:*} failed" >&2; kill $srv_pid; exit 1; }
    fsimg put -m 755 "$SRCDIR/build-${t%%:*}-sage040/sage040/bin/${t##*:}" "/bin/${t##*:}"
done
# The install under test: the port's own target, which brings ca-certs.
make -s -C ../ports/curl install DISK="$DISK" > "$WORK/install.log" 2>&1 || {
    cat "$WORK/install.log"; echo "curltest: make install failed" >&2
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
seen UP || { echo "curltest: the machine never answered"; finish; exit 1; }

H=http://testhost:$HTTP_PORT
IP=http://10.0.2.2:$HTTP_PORT
TS=https://testhost:$HTTPS_PORT
echo "=== running on the machine ==="
run 'ifconfig dhcp' dhcp
run 'cd /ST' cd
run 'export LANG=C.UTF-8' lang
run 'curl --version' version
run "curl -s -o /dev/null http://nosuch.invalid/; echo rc=\$?" invalid 1800
run "curl -s -o small.txt $IP/small.txt; echo rc=\$?" small
run 'openssl dgst -sha256 -r small.txt' small-sum
run "curl -s -o big.bin $H/big.bin; echo rc=\$?" big 1800
run 'openssl dgst -sha256 -r big.bin' big-sum
run "curl -s -L -o redir.txt $H/redir; echo rc=\$?" redir
run 'openssl dgst -sha256 -r redir.txt' redir-sum
run "curl -s --compressed -o gz.bin $H/gz/big.bin; echo rc=\$?" gz 1800
run 'openssl dgst -sha256 -r gz.bin' gz-sum
run "curl -s --data-binary @post.bin $H/post" post
run "curl -s -r 0-99999 -o part.bin $H/big.bin; echo rc=\$?" part 1800
run "curl -s -C - -o part.bin $H/big.bin; echo rc=\$?" resume 1800
run 'openssl dgst -sha256 -r part.bin' resume-sum
run "curl -s --cacert ca.pem -o tls.txt -w 'handshake=%{time_appconnect}\\n' $TS/small.txt; echo rc=\$?" tls 1800
run 'openssl dgst -sha256 -r tls.txt' tls-sum
run "curl -s -o /dev/null $TS/small.txt; echo rc=\$?" tls-default 1800
run "curl -s -k -o /dev/null $TS/small.txt; echo rc=\$?" tls-k 1800
run "curl -s --cacert ca.pem -o /dev/null https://10.0.2.2:$HTTPS_PORT/small.txt; echo rc=\$?" tls-name 1800
run "curl -s --compressed -o br.bin $H/br/big.bin; echo rc=\$?" br 1800
run 'openssl dgst -sha256 -r br.bin' br-sum
run "curl -s --compressed -o zst.bin $H/zstd/big.bin; echo rc=\$?" zst 1800
run 'openssl dgst -sha256 -r zst.bin' zst-sum
if [ "$HAVE_H2" = 1 ]; then
    run "curl -s --cacert ca.pem -o h2.txt -w 'version=%{http_version}\\n' https://testhost:$H2_PORT/small.txt; echo rc=\$?" h2 1800
    run 'openssl dgst -sha256 -r h2.txt' h2-sum
fi
run "curl -s -o idn.txt http://bücher.test:$HTTP_PORT/small.txt; echo rc=\$?" idn 1800
run 'openssl dgst -sha256 -r idn.txt' idn-sum
run "curl -s -c jar.txt -o /dev/null http://www.example.co.uk:$HTTP_PORT/cookie; echo rc=\$?" psl-set 1800
run "curl -s -b jar.txt http://www.example.co.uk:$HTTP_PORT/echo-cookie" psl-get 1800
run 'idn2 bücher.example' idn2
run 'psl --print-unreg-domain www.example.co.uk' psl
run 'brotli -d -c host.br > unbr.bin' unbr 1800
run 'openssl dgst -sha256 -r unbr.bin' unbr-sum
run "openssl s_client -connect 10.0.2.2:$HTTPS_PORT -servername testhost -verify_hostname testhost -CAfile ca.pem -verify_return_error < /dev/null; echo rc=\$?" s_client 1800
if [ "$ONLINE" = 1 ]; then
    run "curl -s -o /dev/null -w 'code=%{http_code} handshake=%{time_appconnect}\\n' https://curl.se/; echo rc=\$?" internet 3000
fi
run 'echo ==END' end
finish

CLEAN="$S/curl-clean.txt"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' > "$CLEAN"

echo "=== guest session ==="
sed -n '/^==version/,$p' "$CLEAN" | grep -v '^echo ' | sed 's/^/  | /'

out(){ sed -n "/^==$1\$/,/^DONE-$1\$/p" "$CLEAN" | sed '1d;$d'; }
rc(){ out "$1" | grep -oE '^rc=[0-9]+' | tail -1 | cut -d= -f2; }

pass=0; fail=0; skip=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }

echo "=== checks ==="
out version | grep -q '^curl 8\.22\.0 .*OpenSSL/3\.5'
check "curl --version names curl 8.22.0 and OpenSSL 3.5" $?
out version | grep -E '^Features:' | grep -qw libz && \
    out version | grep -E '^Features:' | grep -qw zstd
check "  with libz and zstd" $?

[ "$(rc invalid)" = 6 ]
check "a DNS lookup with no resolv.conf answers 'no such name' (exit 6), not a crash" $?

[ "$(rc small)" = 0 ] && out small-sum | grep -qF "$SMALL"
check "http by address: the small file arrives intact" $?
[ "$(rc big)" = 0 ] && out big-sum | grep -qF "$BIG"
check "http by NAME (/etc/hosts): the 2 MB file arrives intact" $?
[ "$(rc redir)" = 0 ] && out redir-sum | grep -qF "$SMALL"
check "-L follows a 302 to the right file" $?

[ "$(rc gz)" = 0 ] && out gz-sum | grep -qF "$BIG"
check "--compressed: gzip on the wire, the original bytes on disk" $?
grep -q 'GET /gz/big.bin .*ae=.*gzip' "$WORK/requests.log"
check "  and curl really asked for gzip (the server's log)" $?

out post | grep -qxF "$POST"
check "a 50 KB POST reaches the server intact (the server's hash)" $?

[ "$(rc resume)" = 0 ] && out resume-sum | grep -qF "$BIG"
check "-C - completes a partial download correctly" $?
grep -q 'GET /big.bin range=bytes=100000-' "$WORK/requests.log"
check "  by asking for the rest, not the whole file again" $?

[ "$(rc tls)" = 0 ] && out tls-sum | grep -qF "$SMALL"
check "https with --cacert: verified, and the file arrives intact" $?
[ "$(rc tls-default)" = 60 ]
check "control: the test CA is NOT in the default store (exit 60)" $?
[ "$(rc tls-k)" = 0 ]
check "control: -k gets through, so that failure was verification" $?
[ "$(rc tls-name)" = 60 ]
check "control: a certificate for the wrong NAME is refused (exit 60)" $?

[ "$(rc s_client)" = 0 ] && out s_client | grep -qF 'Verify return code: 0 (ok)'
check "openssl s_client connects and verifies (FIONBIO)" $?

[ "$(rc br)" = 0 ] && out br-sum | grep -qF "$BIG"
check "--compressed with brotli (br): the original bytes on disk" $?
[ "$(rc zst)" = 0 ] && out zst-sum | grep -qF "$BIG"
check "--compressed with zstd: the original bytes on disk" $?
grep -q 'GET /br/big.bin .*ae=.*\bbr\b' "$WORK/requests.log" && \
    grep -q 'GET /zstd/big.bin .*ae=.*\bzstd\b' "$WORK/requests.log"
check "  and curl offered br and zstd (the server's log)" $?

if [ "$HAVE_H2" = 1 ]; then
    [ "$(rc h2)" = 0 ] && out h2 | grep -qx 'version=2' && out h2-sum | grep -qF "$SMALL"
    check "HTTP/2 to an h2-only server: negotiated, reported as 2, file intact" $?
    grep -q '^H2 /small.txt' "$WORK/requests.log"
    check "  and the server parsed it as HTTP/2 frames (its log)" $?
else
    echo "  [SKIP] HTTP/2 -- the host has no python h2 (requirements.txt)"; skip=$((skip+1))
fi

[ "$(rc idn)" = 0 ] && out idn-sum | grep -qF "$SMALL"
check "IDN: http://bücher.test/ fetched with LANG=C.UTF-8" $?
grep -q "GET /small.txt .*host=xn--bcher-kva.test:$HTTP_PORT" "$WORK/requests.log"
check "  and it arrived as xn--bcher-kva.test (the server's Host header)" $?

out psl-get | grep -q 'site=1'
check "PSL: a cookie for example.co.uk is kept" $?
out psl-get | grep -q 'cookie=' && ! out psl-get | grep -q 'wide=1'
check "PSL: a cookie for all of co.uk is REFUSED" $?

[ -n "$H_IDN" ] && out idn2 | grep -qxF "$H_IDN"
check "idn2 on the machine agrees with the host's ($H_IDN)" $?
[ -n "$H_PSL" ] && out psl | grep -qxF "$H_PSL"
check "psl on the machine agrees with the host's ($H_PSL)" $?
out unbr-sum | grep -qF "$BIG"
check "brotli on the machine decodes the host's brotli output exactly" $?

if [ "$ONLINE" = 1 ]; then
    [ "$(rc internet)" = 0 ] && out internet | grep -q 'code=200'
    check "https://curl.se verifies against the Mozilla bundle" $?
else
    echo "  [SKIP] https://curl.se -- the host is offline"; skip=$((skip+1))
fi

echo
echo "  TLS handshake: $(out tls | grep -oE 'handshake=[0-9.]+' | cut -d= -f2) s (local, RSA-2048)"
[ "$ONLINE" = 1 ] && \
echo "  TLS handshake: $(out internet | grep -oE 'handshake=[0-9.]+' | cut -d= -f2) s (curl.se, total incl. connect)"
echo
echo "  passed: $pass"
echo "  failed: $fail"
[ "$skip" -gt 0 ] && echo "  skipped: $skip"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
