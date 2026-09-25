#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# homeenvtest.sh - a person can write to their own home, and their
# shell startup files actually run when they log in.
#
# Two bugs hid here, both of which passed every earlier test because no
# suite logged in as an ordinary user and then TRIED TO USE the account:
#
#   - The home directory was created by `mkdir` and never chowned, so it
#     belonged to root. A user cannot create a file in a directory root
#     owns, and the symptom is "I have to sudo to write to my own home".
#     The kernel was right to refuse; the setup was wrong to leave it
#     root's. install-etc chowns it now.
#
#   - jfrancis logs into bash, and a bash LOGIN shell reads
#     ~/.bash_profile, never ~/.bashrc -- so a personal .bashrc silently
#     did nothing at login until it was sourced by hand. The shipped
#     ~/.bash_profile sources ~/.bashrc, which is the whole fix.
#
# It exercises the REAL install-etc (make -C system install-etc against
# this disk), not a copy, so a regression in that target is caught here.
# The account logs into /bin/bash, because the .bashrc half is bash's.
set -u
cd "$(dirname "$0")"
. ../machine.conf
QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
S=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$S"
DISK="$S/hd-homeenv.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "homeenvtest: fsimg $* failed" >&2; exit 1; }; }
LOG="$S/homeenv.log"; F="$S/homeenv.fifo"
rm -f "$LOG" "$F" "$S/homeenv.state"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh id || exit 1
make -s -C ../auth || exit 1
make -s -C ../ldso || exit 1
[ -x ../ports/bash/bash ]     || ../ports/bash/build.sh  >/dev/null 2>&1 || true
[ -x ../ports/sbase/bin/cat ] || ../ports/sbase/build.sh >/dev/null 2>&1 || true
[ -x ../ports/bash/bash ] || { echo "homeenvtest: no bash port"; echo "RESULT: SKIP"; exit 0; }

rm -f "$DISK"; dd if=/dev/zero of="$DISK" bs=1M count=48 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /etc
fsimg put -m 755 ../system/sh   /bin/sh
fsimg put -m 755 ../system/id   /bin/id
fsimg put -m 755 ../ports/bash/bash /bin/bash
for p in cat ls echo whoami touch rm; do
    fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p 2>/dev/null
done
fsimg put -m 755 ../auth/login /bin/login
fsimg put -m 4755 ../auth/su    /bin/su
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so

# A passwd where jfrancis logs into BASH, put down before install-etc so
# its "install only if missing" leaves it. Everything else -- /home,
# /etc/{group,shadow,profile,sudoers,rc}, the dotfiles, and the chown of
# the home -- comes from the real install-etc.
printf 'root:x:0:0:root:/root:/bin/sh\njfrancis:x:1000:1000:Jeff Francis:/home/jfrancis:/bin/bash\n' \
    > "$S/homeenv-passwd.tmp"
fsimg put "$S/homeenv-passwd.tmp" /etc/passwd

echo "=== running the real install-etc against this disk ==="
make -s -C ../system install-etc DISK="$DISK" || exit 1

echo "=== home ownership after install-etc ==="
fsimg ls-l /home | sed 's/^/  | /'

mkfifo "$F"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
  -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
  -chardev stdio,id=con,signal=off -serial chardev:con \
  < "$F" > "$LOG" 2>&1 &
pid=$!
exec 3> "$F"; sleep 6
send(){ printf '%s\r' "$1" >&3; sleep "${2:-1}"; }

send 'jfrancis' 2          # login name
send 'jfrancis' 5          # password (from the shipped shadow)
send 'echo ==WHO; whoami; id' 2
# .bashrc ran at login iff EDITOR is set from it (via .bash_profile).
send 'echo ==ENV; echo "EDITOR=[$EDITOR] ED=[$ED]"' 2
send 'echo ==ALIAS; alias' 2
# The write that used to need sudo.
send 'echo ==WRITE; touch /home/jfrancis/hello && echo WROTE-OK || echo WRITE-FAILED' 3
send 'echo ==LSHOME; ls -la /home/jfrancis/hello' 2
# THE SAME WRITE BY A BARE RELATIVE NAME, which is how a person actually
# types it: `touch hello2`, not the absolute path. This is a SEPARATE
# code path -- the permission check computes the parent directory from
# the name, and a bare name's parent is the current directory, not the
# root. It once checked "/" for every relative creation, so a user could
# make files in its own home ONLY by absolute path; the absolute WRITE
# above passed while this failed with "Permission denied".
send 'echo ==RELWRITE; cd /home/jfrancis; touch hello2 && echo REL-WROTE-OK || echo REL-WRITE-FAILED' 3
send 'echo ==LSREL; ls -la hello2' 2
send 'echo ==DONE' 2
for i in $(seq 1 60); do grep -q '==DONE' "$LOG" && break; sleep 1; done
sleep 1
exec 3>&-; kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$F"
CLEAN="$S/homeenv-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' > "$CLEAN"

echo "=== guest session ==="
sed -n '/==WHO/,$p' "$CLEAN" | sed 's/^/  | /'

pass=0; fail=0
check(){ if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass+1));
         else echo "  [FAIL] $1"; fail=$((fail+1)); fi; }
has(){ grep -qF "$1" "$CLEAN"; }

echo "=== checks ==="
grep -qE '^\s+[0-9]+\s+40755.*\s1000\s+1000\s.*jfrancis' <(fsimg ls-l /home)
check "install-etc left /home/jfrancis owned by jfrancis (1000:1000)" $?

has 'uid=1000(jfrancis)'
check "logged in as jfrancis over bash" $?

sed -n '/==ENV/,/==ALIAS/p' "$CLEAN" | grep -q 'EDITOR=\[em\]'
check ".bashrc ran at login -- EDITOR=em came from it via .bash_profile" $?
sed -n '/==ALIAS/,/==WRITE/p' "$CLEAN" | grep -q "l='\?ls -alF"
check "  and its alias took effect too" $?

has 'WROTE-OK'
check "jfrancis can create a file in its own home -- no sudo" $?
sed -n '/==LSHOME/,/==RELWRITE/p' "$CLEAN" | grep -q 'hello'
check "  and the file is really there" $?

has 'REL-WROTE-OK'
check "  and by a bare relative name too -- parent is the cwd, not /" $?
sed -n '/==LSREL/,/==DONE/p' "$CLEAN" | grep -q 'hello2'
check "  and that file is really there as well" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
