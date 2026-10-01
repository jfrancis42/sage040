#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# procpstest.sh - procps-ng on the machine: ps, top, free, pgrep, pkill,
# pidof, pmap, pwdx, vmstat, uptime, w, kill -- and the login records
# and signal rules they lean on.
#
# Every one of these reads /proc the way it is on Linux, so this is as
# much a test of kernel/procfs.c as of the port. The answers are checked
# against facts the test knows another way: the pid the shell reported
# for a background job (`$!`), the machine's configured memory, the
# directory the test put the shell in, the number of people the test
# logged in. A field that merely "looks like a number" proves nothing.
#
# It logs in on the console through /bin/login, so that the session is
# in /var/run/utmp (login writes it now): uptime and w must count one
# user, and after logout none. libc/test/sigqtest checks the signal
# rules the kill and pkill here depend on.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-procps.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/procpstest.log"; FIFO="$SCRATCH/procps.fifo"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "procpstest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
POUT=${SAGE_SRC:-$HOME/m68k/src}/build-procps-sage040/sage040/bin

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../auth || exit 1
make -s -C ../libc/test sigqtest >/dev/null || exit 1
SAGE_LIBC=$SAGE_LIBC ../ports/procps/build.sh >/dev/null || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /etc /lib /root /home /var /var/run /var/log /tmp; do fsimg mkdir $d; done
put_shells
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../auth/login /bin/login
fsimg put ../system/passwd /etc/passwd
fsimg put ../system/group /etc/group
fsimg put -m 600 ../system/shadow /etc/shadow
fsimg put ../system/profile /etc/profile
for p in cat ls echo sleep true; do fsimg put -m 755 ../ports/sbase/bin/$p /bin/$p; done
for p in ps top free pgrep pkill pidof pmap pwdx vmstat uptime w kill; do
    fsimg put -m 755 "$POUT/$p" /bin/$p
done
fsimg put -m 755 ../libc/test/sigqtest /sigqtest
: > "$SCRATCH/wtmp.tmp"; fsimg put "$SCRATCH/wtmp.tmp" /var/log/wtmp

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
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_for "$1-END [0-9]+" "${3:-600}"; }

# The console runs /bin/login when there is a passwd; log in as root.
wait_for 'login: ?' 900
printf 'root\r' >&3; sleep 2; printf 'root\r' >&3
wait_for '.*[#$] ?' 300
sleep 1
run START 'echo started'
# The system shell says a background job's pid as it starts it -- "[9]
# /bin/sleep 300 &" -- which is the independent source every check of
# it is made against.
run SLEEP '/bin/sleep 300 &'
bg=$(tr -d '\r' < "$LOG" | sed -n 's|^\[\([0-9]*\)\] */bin/sleep 300 *&$|\1|p' | tail -1)
run PS '/bin/ps -eo pid,ppid,user,stat,comm'
run PSF "/bin/ps -p ${bg:-0} -o pid=,comm=,user="
run PGREP '/bin/pgrep -x sleep'
run PIDOF '/bin/pidof sleep'
run FREE '/bin/free -k'
run TOP '/bin/top -b -n 1 -w 200'
run VMSTAT '/bin/vmstat'
run CD 'cd /tmp; /bin/pwdx $$; cd /'
run PMAP '/bin/pmap $$'
run UPTIME '/bin/uptime'
run W '/bin/w -h'
run SIGQ '/sigqtest' 1200
run PKILL '/bin/pkill -x sleep; /bin/sleep 1; /bin/pgrep -x sleep; echo "PG=$?"'
run KILLL '/bin/kill -l TERM'
printf 'exit\r' >&3
wait_for 'login: ?' 300
sleep 1
printf 'root\r' >&3; sleep 2; printf 'root\r' >&3
wait_for '.*[#$] ?' 300
sleep 1
run UPTIME2 '/bin/uptime'
run UTMP '/bin/ls -l /var/run/utmp /var/log/wtmp'
printf 'exit\r' >&3
sleep 2
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CL="$SCRATCH/procps-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' > "$CL"
echo "=== guest session ==="
sed -n '/started/,$p' "$CL" | sed 's/^/  | /' | head -150
block() { sed -n "/echo \"$1\"-END/,/^$1-END/p" "$CL"; }
status() { sed -n "s/^$1-END //p" "$CL" | tail -1; }

echo "=== checks ==="
[ -n "$bg" ]; check "a background sleep, pid ${bg:-?}" $?
block PS | grep -qE "^ *$bg +[0-9]+ +root +S +sleep\$"
check "ps -eo lists it: that pid, root, sleeping, named sleep" $?
block PS | grep -qE '^ *[0-9]+ +[0-9]+ +root +[RS]+ +ps$'
check "  and lists itself, running" $?
[ "$(block PSF | grep -cE "^ *$bg +sleep +root\$")" = 1 ]
check "ps -p PID picks out exactly it" $?
block PGREP | grep -qx " *$bg" && block PIDOF | grep -qx " *$bg"
check "pgrep -x and pidof find the same pid" $?
total=$(block FREE | awk '/^Mem:/ {print $2}')
[ -n "$total" ] && [ "$total" -le $((RAM_MB * 1024)) ] && [ "$total" -ge $((RAM_MB * 1024 * 3 / 4)) ]
check "free: the total memory is the machine's ${RAM_MB} MB, less the kernel (${total:-?} KB)" $?
block TOP | grep -qE '^top - ' && block TOP | grep -qE '^Tasks: +[0-9]+ total' &&
    block TOP | grep -qE "^ *$bg +root .*sleep( 300)? *\$"
check "top -b: its header, a task count, and the sleep among the processes" $?
block VMSTAT | grep -qE '^ *[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+'
check "vmstat prints a line of figures" $?
block CD | grep -qE '^[0-9]+: /tmp$'
check "pwdx names the shell's working directory, /tmp" $?
block PMAP | grep -qE '^ *total +[0-9]+K$'
check "pmap of the shell maps it and adds it up" $?
block UPTIME | grep -qE 'up .* 1 user'
check "uptime counts one user -- the console login, from utmp" $?
block W | grep -qE '^root +console'
check "w lists root on the console" $?
grep -qE '^sigqtest: [0-9]+ checks, 0 failed$' "$CL"
check "sigqtest: sigqueue, the sender's pid, and who may signal whom" $?
grep -qx 'PG=1' "$CL"
check "pkill -x sleep ends it: pgrep finds nothing after" $?
block KILLL | grep -qx '15'
check "kill -l TERM is 15" $?
block UPTIME2 | grep -qE 'up .* 1 user'
check "after logging out and in again, still one user: the first session was marked ended" $?
# A struct utmp is 382 bytes here: m68k aligns an int to two bytes, so
# there is no padding after ut_type.
block UTMP | grep -qE ' 382 .*/var/run/utmp$'
check "utmp holds the console's one slot, rewritten in place (382 bytes)" $?
block UTMP | grep -qE ' 1146 .*/var/log/wtmp$'
check "wtmp has three records: a login, its logout, the second login" $?
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
