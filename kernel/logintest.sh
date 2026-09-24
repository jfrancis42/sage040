#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# logintest.sh - logging in, and being somebody in particular.
#
# The console asks for a name and a password; the right password gets a
# shell in that user's home directory, and the wrong one does not. Then
# the three things that only mean anything once that works:
#
#   - a NON-ROOT user is refused root's 0600 file. Every other suite
#     runs as root, where the permission checks are bypassed by design,
#     so this is the only place the enforcement is actually proven.
#   - sudo, with NOPASSWD, gets it anyway -- and that is set-user-id
#     working end to end: the program starts with euid 0 because of a
#     bit on the inode, and gives it back when it is done.
#   - useradd makes an account that can then be logged into, which is
#     the whole of /etc/passwd, /etc/shadow and /etc/group being
#     written correctly rather than plausibly.
#
# Runs on a scratch image of its own.
set -u
cd "$(dirname "$0")"
. ../machine.conf
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
S=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}; mkdir -p "$S"
DISK="$S/hd-login.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "logintest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/logintest.log"; rm -f "$LOG" "$S/li.fifo"

# BUILD WHAT THIS NEEDS, like every other suite. Without it a missing
# binary is an fsimg line saying "no such file" in a wall of setup
# output, and then a run whose failures look like the kernel: this
# suite once reported "the console asked for a login: FAIL" when the
# truth was that ../auth/login did not exist.
echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh id env || exit 1
make -s -C ../auth || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || true
rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /etc; fsimg mkdir /lib; fsimg mkdir /home; fsimg mkdir /root
fsimg put -m 755 ../system/sh /bin/sh
for p in id env; do fsimg put -m 755 ../system/$p /bin/$p; done
for p in cat ls echo whoami touch rm mkdir chmod; do
  fsimg put -m 755 ../ports/sbase/bin/$p /bin/$p 2>/dev/null
done
fsimg put -m 755 ../auth/login   /bin/login
fsimg put -m 755 ../auth/useradd /bin/useradd
fsimg put -m 755 ../auth/userdel /bin/userdel
fsimg put -m 4755 ../auth/su     /bin/su
fsimg put -m 4755 ../auth/sudo   /bin/sudo
fsimg put -m 4755 ../auth/passwd /bin/passwd
fsimg put ../system/passwd /etc/passwd
fsimg put ../system/group  /etc/group
fsimg put -m 600 ../system/shadow /etc/shadow
fsimg put -m 440 ../system/sudoers /etc/sudoers
fsimg put ../system/profile /etc/profile
fsimg put ../system/dot-profile /root/.profile
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg mkdir /home/jfrancis
echo "secret-of-root" > "$S/rootfile.tmp"; fsimg put -m 600 "$S/rootfile.tmp" /root/secret.txt
echo "=== modes on disk ==="; fsimg ls-l /bin | grep -E ' (su|sudo|passwd|login)$'
mkfifo "$S/li.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
  -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
  -chardev stdio,id=con,signal=off -serial chardev:con \
  < "$S/li.fifo" > "$LOG" 2>&1 &
pid=$!
exec 3> "$S/li.fifo"
sleep 6
send(){ printf '%s\r' "$1" >&3; sleep "${2:-1}"; }
send 'root' 1          # the login prompt
send 'root' 3          # the password
send 'whoami'
send 'id'
send 'cat /root/secret.txt'
send 'su - jfrancis' 1
send 'jfrancis' 3
send 'whoami'
send 'cat /root/secret.txt'   # must be REFUSED now
send 'sudo -l' 2
send 'sudo cat /root/secret.txt' 3   # NOPASSWD: must work
send 'exit' 2
send 'useradd -p hunter2 tester' 3
send 'su - tester' 1
send 'hunter2' 3
send 'whoami'
send 'pwd'
send 'exit' 2
send 'echo ALL-DONE' 2
for i in $(seq 1 60); do grep -q 'ALL-DONE' "$LOG" && break; sleep 1; done
sleep 2
exec 3>&-; kill $pid 2>/dev/null; wait $pid 2>/dev/null
CLEAN="$S/logintest-clean.tmp"
tr -d '\r' < "$LOG" > "$CLEAN"

echo "=== guest session ==="
sed -n '/kernel ready/,$p' "$CLEAN" | sed 's/^/  | /'

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }
has(){ grep -qF "$1" "$CLEAN"; }

echo "=== checks ==="
has 'login:'                         ; check "the console asked for a login" $?
grep -q '^/root\$ whoami' "$CLEAN"   ; check "  a correct password got a shell in /root" $?
has 'uid=0(root)'                    ; check "  as root" $?
has 'secret-of-root'                 ; check "root reads its own 0600 file" $?
has '/home/jfrancis$ whoami'         ; check "su - became jfrancis, in that home directory" $?
has '/root/secret.txt: permission denied'
check "  and jfrancis is REFUSED root's 0600 file -- enforcement, as a user" $?
has 'without a password'             ; check "sudo -l reports the NOPASSWD rule" $?
# sudo must have actually produced the contents, not just been allowed.
[ "$(grep -c 'secret-of-root' "$CLEAN")" -ge 2 ]
check "  and sudo cat read the file jfrancis could not" $?
has 'profile: welcome to sage040'
check "the login shell ran /etc/profile and ~/.profile" $?
has 'added tester'                   ; check "useradd created an account" $?
has '/home/tester$ whoami'           ; check "  which can then be logged into, in its own home" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
