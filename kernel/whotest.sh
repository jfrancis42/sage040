#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# whotest.sh - who(1) and w(1): is the machine's answer about who is
# logged in actually right?
#
# THIS SUITE EXISTS BECAUSE THE FIRST RULE WAS WRONG AND LOOKED FINE.
# who(1) originally counted a login as "a session leader with a
# terminal", which is what a Unix with sessions would say -- and on
# this machine, where nothing calls setsid(), it reported the kernel's
# own `idle` and `netd` tasks as two logged-in roots while missing the
# person actually at the keyboard. The output was a plausible three
# lines of plausible columns. Nothing but looking at it found that, so
# the checks below are about WHICH tasks are named and which are not,
# and never about the shape of the output.
#
# TWO KINDS OF LOGIN, because they arrive by completely different
# paths and the rule has to cover both: somebody at the console, who
# got there through /bin/login, and somebody over ssh, who got there
# through Dropbear -- which is built --disable-utmp and never goes
# near login. A suite that tested only the console would have passed
# against a `who` that could not see a single remote user.
#
# AND THE NEGATIVE, which is the half that matters: the machine is
# made to have tasks on the console that are NOT people -- klogd,
# backgrounded by /etc/rc and holding the console open for life -- and
# the suite fails if `who` names one.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
DBOUT=$SRCDIR/build-dropbear-sage040/sage040

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-who.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/whotest.log"
WORK="$SCRATCH/who.tmp"
PORT=${WHO_TEST_PORT:-2253}
# A suite must never be able to grade a previous run's log.
rm -f "$LOG"; rm -rf "$WORK"; mkdir -p "$WORK"

pass=0
fail=0
check() {
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}

for t in ssh sshpass; do
    command -v $t > /dev/null || {
        echo "whotest: no $t here"; echo "RESULT: SKIP"; exit 0; }
done

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh who w id env || exit 1
make -s -C ../auth || exit 1
make -s -C ../ldso || exit 1
[ -x "$DBOUT/bin/dropbear" ] || ../ports/dropbear/build.sh > /dev/null || exit 1
[ -x ../ports/sbase/bin/cat ] || ../ports/sbase/build.sh > /dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /etc
fsimg mkdir /root; fsimg mkdir /home; fsimg mkdir /home/jfrancis
fsimg mkdir /var; fsimg mkdir /var/log
for p in sh who w id env klogd ifconfig; do
    fsimg put -m 755 "../system/$p" /bin/$p
done
fsimg put -m 755 ../auth/login /bin/login
fsimg put -m 4755 ../auth/sudo /bin/sudo
fsimg put -m 4755 ../auth/su   /bin/su
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
for b in dropbear dropbearkey; do
    fsimg put -m 755 "$DBOUT/bin/$b" /bin/$b
done
for p in $(ls ../ports/sbase/bin); do
    fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p 2>/dev/null
done
fsimg put ../system/passwd /etc/passwd
fsimg put ../system/group  /etc/group
fsimg put -m 600 ../system/shadow /etc/shadow
fsimg put -m 440 ../system/sudoers /etc/sudoers
fsimg chown 1000 1000 /home/jfrancis

# klogd, and nothing else: a task on the console that is not a person,
# so that "who lists only people" is a claim with something to fail on.
printf 'klogd -q &\n' > "$WORK/rc"
fsimg put "$WORK/rc" /etc/rc

rm -f "$SCRATCH/who.fifo"; mkfifo "$SCRATCH/who.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -nic user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/who.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/who.fifo"
sleep "${BOOT_WAIT:-8}"
send() { printf '%s\r' "$1" >&3; sleep "${2:-2}"; }

# --- somebody at the console ----------------------------------------
send 'jfrancis' 1
send 'jfrancis' 5
send 'echo ==A1'; send 'who' 3; send 'echo ==A2'
send 'echo ==B1'; send 'w' 3;   send 'echo ==B2'

# --- and somebody over ssh, on a pty ---------------------------------
send 'sudo ifconfig dhcp' 15
send 'sudo dropbearkey -t ed25519 -f /etc/hostkey' 45
send 'sudo dropbear -r /etc/hostkey -p 22 -E &' 8

SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
SSHO="$SSHO -o PreferredAuthentications=password -o PubkeyAuthentication=no"
SSHO="$SSHO -o ConnectTimeout=15 -o LogLevel=ERROR -p $PORT"
# A REAL LOGIN SHELL, not `ssh host command`. The two are different on
# purpose and the difference is exactly what who(1) keys on: Dropbear
# starts an interactive session with argv[0] of "-sh" and a remote
# command with a plain "sh", so a remote command is NOT somebody logged
# in and must not be listed. Testing the second would have proved
# nothing about the first.
#
# Fed from a fifo rather than a pipe, so the session stays up while the
# console is asked who is logged in -- a pipe closes when the feeding
# subshell exits and the shell would see end of input at once.
rm -f "$WORK/sshin"; mkfifo "$WORK/sshin"
( sshpass -p jfrancis ssh $SSHO -tt jfrancis@127.0.0.1 \
      < "$WORK/sshin" > "$WORK/ssh.out" 2>&1 ) &
ssh_pid=$!
exec 4> "$WORK/sshin"
sleep 8
printf 'echo SSH-IN\r' >&4
for i in $(seq 1 40); do
    grep -q 'SSH-IN' "$WORK/ssh.out" 2>/dev/null && break
    sleep 1
done
send 'echo ==C1'; send 'who' 4;    send 'echo ==C2'
send 'echo ==D1'; send 'w' 4;      send 'echo ==D2'
send 'echo ==E1'; send 'who -a' 4; send 'echo ==E2'

# --- and what it looks like once that session has gone ---------------
printf 'exit\r' >&4
sleep 3
exec 4>&-
kill $ssh_pid 2>/dev/null; wait $ssh_pid 2>/dev/null
sleep 5
send 'echo ==F1'; send 'who' 4; send 'echo ==F2'
send 'echo ==DONE' 3

for i in $(seq 1 60); do grep -q '==DONE' "$LOG" && break; sleep 1; done
sleep 2
exec 3>&-
kill $qemu_pid 2>/dev/null; wait $qemu_pid 2>/dev/null
rm -f "$SCRATCH/who.fifo"

CLEAN="$WORK/clean.txt"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\[?2004[hl]//g' \
    > "$CLEAN"

echo "=== guest session ==="
sed -n '/==A1/,$p' "$CLEAN" | sed 's/^/  | /'

# THE OUTPUT OF ONE COMMAND, taken between two markers the session
# echoed around it. By marker and not by line number, and not by
# matching the shell's prompt either -- the first version of this
# matched the prompt with `\$ w$`, awk warned that it was dropping the
# backslash, and `$ w$` is "end of line, then a space, then w, then end
# of line", which nothing can ever match. So every extraction ran to
# the end of the log, and a check for "exactly one person" was really
# counting the whole session. It failed, which is the lucky case; had
# it been a check for "at least one" it would have passed on anything.
between() {   # between START END -> the lines strictly between them
    awk -v a="$1" -v b="$2" '
        index($0, a) { on = 1; next }
        index($0, b) { on = 0 }
        on' "$CLEAN"
}
# What `who` printed: the lines between the markers, without the
# heading, the echoed command line, the marker coming back, or anything
# the machine said on its own.
#
# THAT LAST ONE IS NOT PARANOIA. The shell announces a finished job --
# "[10]  sudo dropbear ... &" -- and Dropbear logs to the console
# because it was started with -E, and either can land between the
# marker and the output being graded. One of them did: a check for
# "exactly two people are logged in" counted four lines and failed
# against a `who` that had printed precisely the right two.
body() {      # body START END
    between "$1" "$2" \
        | grep -v '^USER ' \
        | grep -v '\$ ' \
        | grep -v '^\[' \
        | grep -v '==' \
        | sed '/^$/d'
}
lines() { echo "$1" | grep -c . ; }

echo "=== checks ==="

# --- the console alone ----------------------------------------------
C=$(body '==A1' '==A2')
[ -n "$C" ]; check "who printed something at all" $?
[ "$(lines "$C")" -eq 1 ]
check "one person is logged in at the console, and who says one" $?
echo "$C" | grep -q '^jfrancis '
check "  and it is jfrancis, who is the one who logged in" $?
echo "$C" | grep -q ' tty '
check "  on the console terminal" $?

# THE NEGATIVE, and the half that matters. These are the tasks the
# first rule got wrong: it named idle and netd and missed the person.
! echo "$C" | grep -qE 'idle|netd'
check "the kernel's own tasks are NOT somebody logged in" $?
! echo "$C" | grep -q '^root '
check "  and nobody is logged in as root, because nobody is" $?
! echo "$C" | grep -q 'klogd'
check "  nor is klogd, which holds the console and is not a person" $?
# klogd must be RUNNING, or excluding it proves nothing at all.
grep -q 'klogd' "$CLEAN"
check "  -- and klogd really is running, so that was a real exclusion" $?

W=$(body '==B1' '==B2')
echo "$W" | grep -q '1 user'
check "w's header counts the one user" $?
echo "$W" | grep -q '^jfrancis .* tty '
check "  and names jfrancis at the console" $?

# --- who -a is a different question and must answer more -------------
A=$(body '==E1' '==E2')
[ "$(lines "$A")" -gt "$(lines "$C")" ]
check "who -a lists more than who does -- tasks, not logins" $?
echo "$A" | grep -q 'klogd'
check "  klogd among them, which is why who did not show it" $?
echo "$A" | grep -q 'idle'
check "  and idle, which is the kernel's own" $?
echo "$A" | grep -q -- '-sh'
check "  and the login shell's argv[0] still carries login's dash" $?

# --- SOMEBODY OVER SSH, WHICH THIS MACHINE CANNOT YET HAVE -----------
#
# Dropbear refuses an interactive session: "ttyname fails for openpty
# device". ttyname() in picolibc reads /proc/self/fd/N, this machine
# has no /proc, and nothing else can name the terminal a descriptor is
# open on -- so `ssh host command` works (sshtest covers ten of them)
# and `ssh host` does not. That is a defect in the machine and not in
# who(1), it is written up in progress.md, and it is why the remote
# half of this suite is a NOTE rather than a failure: a suite that goes
# red for a thing already known and recorded stops being read.
#
# WHAT IS STILL CHECKED IS THAT THE REASON HAS NOT CHANGED. If the ssh
# login starts working, the note becomes a failure and these become
# real checks -- which is the point of testing for the symptom rather
# than skipping the section.
echo "=== the remote half ==="
if grep -q 'SSH-IN' "$WORK/ssh.out" 2>/dev/null; then
    B=$(body '==C1' '==C2')
    [ "$(lines "$B")" -eq 2 ]
    check "two people are logged in now, and who says two" $?
    echo "$B" | grep -q 'pts/'
    check "  the remote one is on a pseudo-terminal" $?
    [ "$(echo "$B" | grep -c '^jfrancis ')" -eq 2 ]
    check "  and both are jfrancis, at the console and over ssh" $?
    D=$(body '==D1' '==D2')
    echo "$D" | grep -q '2 users'
    check "w's header counts both of them" $?
    echo "$D" | grep -q 'pts/'
    check "  and names the remote one on its pseudo-terminal" $?
    F=$(body '==F1' '==F2')
    [ "$(lines "$F")" -eq 1 ]
    check "the ssh session ending takes it off the list" $?
else
    check "the ssh login got in" 1
    echo "         Three separate defects used to stop this, and each"
    echo "         one hid the next: ttyname() had no answer without"
    echo "         /proc, chown could not resolve a /dev path for a"
    echo "         non-root process, and a pty was not owned by whoever"
    echo "         allocated it. See progress.md, and $WORK/ssh.out." 
fi

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
