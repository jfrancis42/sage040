#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# shtest.sh - /bin/sh is bash; the system shell is /bin/msh.
#
# The disk is laid out by the REAL install rules, run against a scratch
# image with DISK= -- not by this suite imitating them -- because what
# is being tested is those rules:
#
#   system alone        /bin/msh, and /bin/sh -> msh, so a disk with
#                       no ports still has a shell for logins
#   then bash           /bin/sh -> bash
#   then system again   /bin/sh still -> bash: reinstalling the
#                       programs must not put the system shell back
#
# The links are read on the HOST, with debugfs. Then the machine boots
# and /bin/sh has to behave as bash in POSIX mode: a #!/bin/sh script's
# backslash-newline continuation (which the system shell ran as a
# newline, and which is why this change was made), and system() from
# a program -- awk's -- reaching bash through /bin/sh.

set -u
cd "$(dirname "$0")"
. ../machine.conf

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-sh.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "shtest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/shtest.log"; FIFO="$SCRATCH/sh.fifo"; T="$SCRATCH/sh.tmp"
rm -f "$LOG" "$FIFO" "$DISK"; rm -rf "$T"; mkdir -p "$T"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
# What a symlink on the image points at, from the host: debugfs prints
# a fast link's target as `Fast link dest: "bash"`.
linkdest() {
    debugfs -R "stat $1" "$DISK?offset=$OFF" 2>/dev/null \
        | sed -n 's/^Fast link dest: "\(.*\)"$/\1/p'
}
inst() {                        # inst DIR: that directory's install rule
    make -s -C "$1" install DISK="$DISK" DISK_MB=64 >> "$T/install.log" 2>&1 || {
        echo "shtest: make -C $1 install failed" >&2; tail -20 "$T/install.log" >&2
        exit 1; }
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1
make -s -C ../ports/awk >/dev/null 2>&1 || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

echo "=== installing with the real rules ==="
inst .                          # kernel/: KERNEL.ROM
inst ../ldso
inst ../system
l1=$(linkdest /bin/sh)
[ "$l1" = msh ] && fsimg exists /bin/msh
check "the system alone: /bin/msh, and /bin/sh -> msh ($l1)" $?
inst ../ports/bash
l2=$(linkdest /bin/sh)
[ "$l2" = bash ]
check "installing bash makes /bin/sh -> bash ($l2)" $?
inst ../system
l3=$(linkdest /bin/sh)
[ "$l3" = bash ]
check "reinstalling the system leaves /bin/sh -> bash ($l3)" $?
inst ../ports/awk
inst ../ports/sbase

# A #!/bin/sh script whose first command continues onto a second line.
printf '#!/bin/sh\necho CONT one \\\n    two\n' > "$T/cont"
fsimg mkdir /t
fsimg put -m 755 "$T/cont" /t/cont
printf 'BEGIN { system("echo AWK=${BASH_VERSION:+bash}") }\n' > "$T/sys.awk"
fsimg put "$T/sys.awk" /t/sys.awk
# Which shell, and whether it is in POSIX mode -- which bash enters when
# it is run by the name sh.
printf 'echo "BV=${BASH_VERSION:+bash} $(shopt -qo posix && echo posix)"\n' > "$T/bv"
fsimg put "$T/bv" /t/bv
printf 'echo MSH-RAN\n' > "$T/m.sh"
fsimg put "$T/m.sh" /t/m.sh

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"
wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() {                         # run TAG COMMAND
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" 300
}

sleep 5
run BV '/bin/sh /t/bv'
run CONT '/t/cont'
run AWK '/bin/awk -f /t/sys.awk'
run MSH '/bin/msh /t/m.sh'
printf 'echo ALL-DONE\r' >&3
wait_for 'ALL-DONE' 100
printf 'halt\r' >&3
sleep 2
exec 3>&-; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
rm -f "$FIFO"
CLEAN="$SCRATCH/shtest-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[?0-9;]*[a-zA-Z]//g' > "$CLEAN"

echo "=== checks: on the machine ==="
grep -aqx 'BV=bash posix' "$CLEAN"
check "/bin/sh is bash, and in POSIX mode" $?
grep -aqx 'CONT one two' "$CLEAN"
check "a #!/bin/sh script's backslash-newline continues the line" $?
grep -aqx 'AWK=bash' "$CLEAN"
check "system() from a program reaches bash through /bin/sh" $?
grep -aqx 'MSH-RAN' "$CLEAN" && grep -aqx 'MSH-END 0' "$CLEAN"
check "the system shell still runs, as /bin/msh" $?
grep -aqE 'panic|bus error|address error|DOUBLE' "$CLEAN"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
