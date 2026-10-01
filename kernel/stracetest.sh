#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# stracetest.sh - strace on the machine (ports/strace, over ptrace).
#
# libc/test/stracee makes a known list of system calls, and strace's
# account of them is checked line by line: the string a write was
# given (read out of the tracee's memory), the error an open returned
# (a register, and the errno decoded), a pid that has to be the
# tracee's own. Then -f follows a fork, -c counts, and -p attaches to a
# process that is already running and leaves it running when strace is
# stopped.

set -u
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-strace.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "stracetest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/stracetest.log"; FIFO="$SCRATCH/strace.fifo"
rm -f "$LOG" "$FIFO"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
make -s -C ../libc/test stracee || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
../ports/strace/build.sh >/dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=24 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /usr; fsimg mkdir /usr/bin; fsimg mkdir /tmp
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
put_shells bash
for p in cat sleep kill true; do fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p; done
fsimg put -m 755 "$SRCDIR/build-strace-sage040/sage040/bin/strace" /usr/bin/strace
fsimg put -m 755 ../libc/test/stracee /stracee

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
        sleep 0.2
    done
    return 1
}
run() {                         # run TAG COMMAND
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" 1500 || echo "    (timed out: $1)"
}

S=/usr/bin/strace
sleep 5
run PLAIN "$S -o /tmp/s1 /stracee"
run SHOW1 "/bin/cat /tmp/s1"
run FOLLOW "$S -f -o /tmp/s2 /stracee"
run SHOW2 "/bin/cat /tmp/s2"
run COUNT "$S -c -o /tmp/s3 /stracee"
run SHOW3 "/bin/cat /tmp/s3"
run FILTER "$S -e trace=openat,write -o /tmp/s5 /stracee"
run SHOW5 "/bin/cat /tmp/s5"
# -p: attach to a process already running, then stop strace and see
# that the process carries on.
run ATTACH "/bin/bash -c '/stracee wait > /tmp/ticks & P=\$!; sleep 2; $S -p \$P -o /tmp/s4 & S=\$!; sleep 4; kill \$S; wait \$S; sleep 2; kill -0 \$P && echo STILL-RUNNING; kill \$P'"
run SHOW4 "/bin/cat /tmp/s4"
printf 'echo ALL-DONE\r' >&3
wait_for 'ALL-DONE' 300
printf 'halt\r' >&3
sleep 2
exec 3>&-; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
rm -f "$FIFO"
CLEAN="$SCRATCH/stracetest-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[?0-9;]*[a-zA-Z]//g' > "$CLEAN"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
st() { sed -n "s/^$1-END //p" "$CLEAN" | head -1; }
out() { awk -v a="$1" -v b="$2" '$0 ~ "^"b"-END" {on=0} on {print} $0 ~ "^"a"-END" {on=1}' "$CLEAN"; }

echo "=== checks ==="
[ "$(st PLAIN)" = 7 ]
check "strace runs the program and exits with its status (7)" $?
s1=$(out PLAIN SHOW1)
echo "$s1" | grep -aqE '^execve\("/stracee", \["/stracee"\], 0x[0-9a-f]+ /\* [0-9]+ vars \*/\) = 0$'
check "execve: the path and argv, read from the tracee" $?
echo "$s1" | grep -aqE '^write\(1, "STRACEE says hello\\n", 19\) += 19$'
check "write: the string out of its memory, and the count back" $?
echo "$s1" | grep -aqE '^openat\(AT_FDCWD, "/no/such/file", O_RDONLY(\|O_LARGEFILE)?\) += -1 ENOENT \(No such file or directory\)$'
check "openat: AT_FDCWD and the flags decoded, -1 ENOENT and its message" $?
gp=$(echo "$s1" | sed -n 's/^getpid() *= \([0-9]*\)$/\1/p' | head -1)
echo "$s1" | grep -aqE "^kill\\($gp, 0\\) += 0\$" && [ -n "$gp" ]
check "getpid's answer is the pid kill() is later given ($gp)" $?
echo "$s1" | grep -aqE '^(wait4|waitpid)\([0-9]+, \[\{WIFEXITED\(s\) && WEXITSTATUS\(s\) == 3\}\], 0(, NULL)?\) += [0-9]+$'
check "wait4: the child's exit status decoded, 3" $?
echo "$s1" | grep -aqE '^exit_group\(7\) += \?$' && echo "$s1" | grep -aqx '+++ exited with 7 +++'
check "and the exit, = ? as Linux shows it (PTRACE_EVENT_EXIT)" $?
! echo "$s1" | grep -aqF 'write(1, "child\n'
check "without -f the child's own write is not shown" $?

s2=$(out FOLLOW SHOW2)
# The child's write may be split by the parent's wait4 between its
# start and its end ("<unfinished ...>" ... "<... write resumed>").
cpid=$(echo "$s2" | sed -n 's/^\([0-9]*\) \+write(1, "child\\n", 6.*/\1/p' | head -1)
[ -n "$cpid" ] && echo "$s2" | grep -aqE "^$cpid +(write\\(1, \"child\\\\n\", 6\\)|<\\.\\.\\. write resumed>\\)) += 6\$" \
    && echo "$s2" | grep -aqE "^[0-9]+ +fork\\(\\) += $cpid\$"
check "-f follows the fork: the child's write, under the pid fork returned ($cpid)" $?
echo "$s2" | grep -aqE '^[0-9]+ +\+\+\+ exited with 3 \+\+\+$'
check "  and the child's exit" $?

s3=$(out COUNT SHOW3)
echo "$s3" | grep -aqE '^ *[0-9.]+ +[0-9.]+ +[0-9]+ +[0-9]+ +1 openat$' \
    && echo "$s3" | grep -aqE ' write$' && echo "$s3" | grep -aqE '^ *100\.00 .* total$'
check "-c: a table, with openat's one error and a total" $?

s5=$(out FILTER SHOW5)
echo "$s5" | grep -aqE '^write\(1, "STRACEE says hello\\n", 19\) += 19$' && echo "$s5" | grep -aq '^openat' && ! echo "$s5" | grep -aq '^getpid'
check "-e trace=openat,write: those, and getpid not" $?

grep -aqx 'STILL-RUNNING' "$CLEAN"
check "-p: the attached process runs on after strace is stopped" $?
s4=$(out ATTACH SHOW4)
echo "$s4" | grep -aqxF 'write(1, "tick\n", 5)                = 5' || echo "$s4" | grep -aqE '^write\(1, "tick\\n", 5\) += 5$'
check "  and strace saw it work while attached" $?

grep -aqE 'panic|bus error|address error|DOUBLE MMU FAULT' "$CLEAN"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
