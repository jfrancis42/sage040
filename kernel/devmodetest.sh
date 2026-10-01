#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# devmodetest.sh - a device's mode is enforced, and a terminal belongs to
# whoever is logged in on it.
#
# A device's owner and mode used to be recorded, shown by ls -l, and
# never consulted on open: any user could open anyone's terminal, or
# write the machine's NVRAM. Now vfs.c checks them as it checks a
# file's, and login(1) gives the terminal to the person logging in and
# takes it back afterwards.
#
# libc/test/devperm opens each device each way and says what happened.
# It is run as jfrancis, logged in on the console through /bin/login,
# and the answers are checked against the device's mode as `ls -l`
# shows it -- and, as a control, the same opens as root succeed.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-devmode.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/devmodetest.log"; FIFO="$SCRATCH/devmode.fifo"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "devmodetest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh id || exit 1
make -s -C ../auth || exit 1
make -s -C ../libc/test devperm >/dev/null || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /etc /lib /root /home /home/jfrancis /var /var/run /tmp; do fsimg mkdir $d; done
fsimg chown /home/jfrancis 1000:1000
put_shells
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../auth/login /bin/login
fsimg put ../system/passwd /etc/passwd
fsimg put ../system/group /etc/group
fsimg put -m 600 ../system/shadow /etc/shadow
for p in ls cat; do fsimg put -m 755 ../ports/sbase/bin/$p /bin/$p; done
fsimg put -m 755 ../libc/test/devperm /bin/devperm
fsimg put -m 755 ../system/id /bin/id

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {
    local i
    for i in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge "${3:-1}" ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_for "$1-END [0-9]+" 600; }
login_as() {                    # login_as NAME PASSWORD NTH-PROMPT
    wait_for 'login: ?' 900 "$3"
    sleep 0.5
    printf '%s\r' "$1" >&3; sleep 2; printf '%s\r' "$2" >&3
    sleep 3
}

DEVS='/dev/console /dev/ttyS0 /dev/nvram /dev/vcsa /dev/klog /dev/null /dev/tty'
login_as jfrancis jfrancis 1
run JID 'id'
run JLS "/bin/ls -l $DEVS"
for d in $DEVS; do run "J$(basename $d)" "/bin/devperm $d r w"; done
printf 'exit\r' >&3
login_as root root 2
run RLS '/bin/ls -l /dev/console'
for d in $DEVS; do run "R$(basename $d)" "/bin/devperm $d r w"; done
printf 'exit\r' >&3
sleep 2
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CL="$SCRATCH/devmode-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' > "$CL"
echo "=== guest session ==="
sed -n '/^uid=/,$p' "$CL" | grep -vE '^$' | sed 's/^/  | /' | head -80
block() { sed -n "/echo \"$1\"-END/,/^$1-END/p" "$CL"; }
# What devperm said about DEVICE in the run tagged WHO (J: jfrancis,
# R: root) -- within that run's block, so that root's answers cannot
# satisfy a check about jfrancis's.
said_as() { block "$1$(basename "$2")" | grep -qx "devperm $2 $3 $4"; }
said() { said_as J "$@"; }

echo "=== checks: as jfrancis, logged in on the console ==="
block JID | grep -q '^uid=1000'
check "logged in as jfrancis" $?
block JLS | grep -qE '^crw--w---- +1 +jfrancis +[a-z0-9]+ .*/dev/console$'
check "/dev/console is jfrancis's now, 0620: login gave it" $?
said /dev/console r OK && said /dev/console w OK
check "  so jfrancis may open it, both ways" $?
block JLS | grep -qE '^crw--w---- +1 +root .*/dev/ttyS0$'
check "/dev/ttyS0, a terminal nobody is logged in on: root's, 0620" $?
said /dev/ttyS0 r EACCES && said /dev/ttyS0 w EACCES
check "  and jfrancis may not open it either way" $?
said /dev/nvram r EACCES && said /dev/nvram w EACCES
check "/dev/nvram (0600, root's): refused both ways" $?
said /dev/vcsa r EACCES
check "/dev/vcsa, the screen's contents (0600): refused" $?
said /dev/klog r OK && said /dev/klog w EACCES
check "/dev/klog (0644): readable, as dmesg needs, and not writable" $?
said /dev/null r OK && said /dev/null w OK
check "/dev/null (0666): anybody, both ways" $?
said /dev/tty r OK && said /dev/tty w OK
check "/dev/tty: one's own terminal, whatever it is" $?

echo "=== checks: as root, after jfrancis logged out ==="
block RLS | grep -qE '^crw--w---- +1 +root .*/dev/console$'
check "/dev/console is root's again: login took it back at logout" $?
n=0
for d in $DEVS; do said_as R "$d" r OK && said_as R "$d" w OK && n=$((n + 1)); done
[ "$n" = 7 ]
check "root opens every one of them both ways ($n of 7) -- the refusals were the mode" $?
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
