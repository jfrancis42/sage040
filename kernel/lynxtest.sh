#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# lynxtest.sh - Lynx on the machine, against kernel/websrv.py on the
# host (10.0.2.2), both as a program (-dump) and as a browser driven
# with keystrokes over the serial console.
#
#   render       -dump turns a page into text, with its links numbered
#                and listed as References -- the text a reader sees,
#                not the source
#   utf-8        a UTF-8 page dumped with LANG=C.UTF-8 keeps "Grüße"
#                and "✓" as the same bytes (wide curses, ncursesw)
#   encodings    a page sent gzip- or brotli-encoded WITHOUT being asked
#                for -- as real servers do, lynx.invisible-island.net
#                among them -- renders the same text. Lynx -dump sends
#                no Accept-Encoding at all (HTTP.c says why); it decodes
#                only if it finds gzip / brotli on PATH, which is why
#                ports/lynx installs both
#   https        with the test CA in SSL_CERT_FILE the page loads; with
#                the default store it is REFUSED, both as shipped and
#                with FORCE_SSL_PROMPT:NO -- which upstream Lynx turned
#                into "accept" (ports/lynx patch 02)
#   internet     if the host is online, Lynx's own site renders. It
#                answers in brotli, which once failed with "Error
#                uncompressing temporary file!" (ports/lynx now links
#                the library). SKIPPED, never passed, when offline
#   browse       interactively: Lynx draws the page, Enter follows the
#                first link and the second page's text appears, q then
#                y leaves -- and the shell answers again afterwards
set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
S=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$S"
DISK="$S/hd-lynx.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "lynxtest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/lynx.log"; FIFO="$S/lynx.fifo"; WORK="$S/lynx.tmp"
rm -f "$LOG" "$FIFO"; rm -rf "$WORK"; mkdir -p "$WORK/www"
HTTP_PORT=${HTTP_PORT:-8093}
HTTPS_PORT=${HTTPS_PORT:-8446}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh ifconfig || exit 1
make -s -C ../ldso || exit 1
../ports/lynx/build.sh > "$WORK/build.log" 2>&1 || {
    tail -20 "$WORK/build.log"; echo "lynxtest: ports/lynx failed" >&2; exit 1; }

for p in $HTTP_PORT $HTTPS_PORT; do
    if ss -lnt 2>/dev/null | grep -q ":$p "; then
        echo "lynxtest: something is already listening on port $p" >&2; exit 1
    fi
done

echo "=== test pages and certificates ==="
cat > "$WORK/www/index.html" <<'EOF'
<html><head><meta charset="utf-8"><title>Sage index</title></head>
<body><h1>Welcome aboard</h1>
<p>Grüße from the host ✓ plain words follow.</p>
<p><a href="page2.html">Second page</a> and <a href="small.txt">a file</a>.</p>
</body></html>
EOF
cat > "$WORK/www/page2.html" <<'EOF'
<html><head><title>Page two</title></head>
<body><p>You followed the link to PAGE-TWO-MARKER.</p></body></html>
EOF
printf 'hello from the host\n' > "$WORK/www/small.txt"

( cd "$WORK" &&
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=lynxtest-ca \
      -keyout ca.key -out ca.pem &&
  openssl req -newkey rsa:2048 -nodes -subj /CN=testhost \
      -keyout srv.key -out srv.csr &&
  printf 'subjectAltName=DNS:testhost\n' > srv.ext &&
  openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
      -days 3650 -extfile srv.ext -out srv.pem ) > "$WORK/certs.log" 2>&1 || {
    cat "$WORK/certs.log"; echo "lynxtest: making certificates failed" >&2; exit 1; }
# A configuration that REFUSES a certificate it cannot verify instead of
# asking -- a -dump has nobody to ask.
printf 'INCLUDE:/etc/lynx.cfg\nFORCE_SSL_PROMPT:NO\n' > "$WORK/strict.cfg"

python3 websrv.py "$WORK/www" "$HTTP_PORT" "$HTTPS_PORT" \
    "$WORK/srv.pem" "$WORK/srv.key" "$WORK/requests.log" 0 > "$WORK/srv.out" 2>&1 &
srv_pid=$!
for _ in $(seq 1 50); do
    curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/small.txt" && break; sleep 0.1
done

ONLINE=0
curl -s -o /dev/null --max-time 10 https://lynx.invisible-island.net/ && ONLINE=1

echo "=== preparing $DISK ==="
rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=48 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /etc; fsimg mkdir /ST; fsimg mkdir /tmp
put_shells
fsimg put -m 755 ../system/ifconfig /bin/ifconfig
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
printf '127.0.0.1 localhost\n10.0.2.2 testhost\n' > "$WORK/hosts"
fsimg put "$WORK/hosts" /etc/hosts
fsimg put "$WORK/ca.pem" /ST/ca.pem
fsimg put "$WORK/strict.cfg" /ST/strict.cfg
make -s -C ../ports/lynx install DISK="$DISK" > "$WORK/install.log" 2>&1 || {
    cat "$WORK/install.log"; echo "lynxtest: make install failed" >&2
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

clean(){ tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\x1b[()][0-9A-B]//g; s/\x1b[=>]//g'; }
seen(){ [ "$(clean | grep -cxF -- "$1")" -ge 1 ]; }
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
seen UP || { echo "lynxtest: the machine never answered"; finish; exit 1; }

H=http://testhost:$HTTP_PORT
TS=https://testhost:$HTTPS_PORT
echo "=== running on the machine ==="
run 'ifconfig dhcp' dhcp
run 'cd /ST' cd
run 'export LANG=C.UTF-8' lang
run 'export TERM=vt100' term
run 'export HOME=/ST' home
run 'lynx -version' version
run "lynx -dump $H/index.html; echo rc=\$?" dump 1800
run "lynx -dump $H/force-gz/index.html; echo rc=\$?" gz 1800
run "lynx -dump $H/force-br/index.html; echo rc=\$?" br 1800
run "export SSL_CERT_FILE=/ST/ca.pem" cafile
run "lynx -dump $TS/page2.html; echo rc=\$?" tls 1800
run "export SSL_CERT_FILE=/etc/ssl/cert.pem" cadefault
run "lynx -dump $TS/page2.html < /dev/null; echo rc=\$?" tls-default 1800
run "lynx -cfg=/ST/strict.cfg -dump $TS/page2.html; echo rc=\$?" tls-no 1800

if [ "$ONLINE" = 1 ]; then
    run "lynx -dump https://lynx.invisible-island.net/; echo rc=\$?" internet 3000
fi

# INTERACTIVE. Lynx draws a full screen of vt100 escapes; the page text
# and the link names are in it. Enter follows the first link; q asks
# whether to quit and y answers.
printf 'echo ==browse\r' >&3
printf 'lynx %s/index.html\r' "$H" >&3
# Wait for the STATUS LINE, which Lynx draws last -- the page text
# appears before Lynx is reading keys, and a key sent then is lost.
for _ in $(seq 1 300); do
    clean | grep -q 'Welcome aboard' && clean | grep -q "q' to quit" && break
    sleep 0.2
done
sleep 3
printf '\n' >&3
for _ in $(seq 1 300); do clean | grep -q 'PAGE-TWO-MARKER' && break; sleep 0.2; done
sleep 2
printf 'q' >&3; sleep 2; printf 'y' >&3; sleep 3
printf 'echo DONE-browse\r' >&3
wait_for DONE-browse 600 || echo "    (lynx did not give the shell back)"
run 'echo ==END' end
finish

CLEAN="$S/lynx-clean.txt"
clean > "$CLEAN"

echo "=== guest session (to the interactive part) ==="
sed -n '/^==version/,/^==browse/p' "$CLEAN" | grep -v '^echo ' | sed 's/^/  | /'

out(){ sed -n "/^==$1\$/,/^DONE-$1\$/p" "$CLEAN" | sed '1d;$d'; }
rc(){ out "$1" | grep -oE '^rc=[0-9]+' | tail -1 | cut -d= -f2; }

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }

echo "=== checks ==="
out version | grep -q 'Lynx Version 2\.9\.3'
check "lynx -version names Lynx 2.9.3" $?
out version | grep -q 'OpenSSL 3\.5'
check "  built with OpenSSL 3.5" $?

[ "$(rc dump)" = 0 ] && out dump | grep -q 'Welcome aboard' && \
    out dump | grep -q 'plain words follow'
check "-dump renders the page as text" $?
out dump | grep -q "http://testhost:$HTTP_PORT/page2.html" && \
    out dump | grep -q 'References'
check "  with its links listed as References" $?
! out dump | grep -q '<p>'
check "  and not as HTML source" $?
out dump | grep -qF 'Grüße' && out dump | grep -qF '✓'
check "UTF-8 survives: Grüße and ✓ are the same bytes (LANG=C.UTF-8)" $?

[ "$(rc gz)" = 0 ] && out gz | grep -q 'Welcome aboard'
check "a page sent gzip-encoded unasked renders the same text" $?
[ "$(rc br)" = 0 ] && out br | grep -q 'Welcome aboard'
check "a page sent brotli-encoded unasked renders the same text" $?

out tls | grep -q 'PAGE-TWO-MARKER'
check "https with the test CA in SSL_CERT_FILE loads the page" $?
! out tls-default | grep -q 'PAGE-TWO-MARKER' && out tls-default | grep -q 'unable to get local issuer'
check "control: with the default store it is REFUSED" $?
! out tls-no | grep -q 'PAGE-TWO-MARKER'
check "control: FORCE_SSL_PROMPT:NO refuses it too (upstream accepted)" $?
grep -c 'GET /page2.html' "$WORK/requests.log" | grep -qx 1 || \
    grep -q 'GET /page2.html' "$WORK/requests.log"
check "  (and the server did see the verified request)" $?

skip=0
if [ "$ONLINE" = 1 ]; then
    [ "$(rc internet)" = 0 ] && out internet | grep -q 'Lynx' && \
        ! out internet | grep -q 'Error uncompressing'
    check "lynx.invisible-island.net renders (brotli, Mozilla bundle)" $?
else
    echo "  [SKIP] lynx.invisible-island.net -- the host is offline"; skip=$((skip+1))
fi

sed -n '/^==browse$/,/^DONE-browse$/p' "$CLEAN" | grep -q 'Welcome aboard'
check "interactive: lynx draws the page on the terminal" $?
sed -n '/^==browse$/,/^DONE-browse$/p' "$CLEAN" | grep -q 'PAGE-TWO-MARKER'
check "  Enter follows the first link to the second page" $?
seen DONE-browse
check "  q, y leaves lynx and the shell answers again" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
