#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# gdbtest.sh - gdb on the machine (ports/gdb, over ptrace).
#
# libc/test/gdbee is built -g -O0 and every answer asked of gdb is fixed
# in its source: a breakpoint in square() is reached with n = 7 three
# calls of nest() deep, square returns 49, the loop's total is 4950, a
# struct holds {12, -34}, a SIGUSR1 is raised and handled, and the
# program exits 5. Each answer comes out of the tracee -- a register,
# its stack, its memory read through PEEKDATA -- so a wrong one is a
# ptrace that read the wrong thing. Then gdb attaches to a process
# already running and lets it go, and gdbserver is driven by gdb over
# loopback, which is the remote protocol end to end on one machine.

set -u
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-gdb.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "gdbtest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/gdbtest.log"; FIFO="$SCRATCH/gdb.fifo"
rm -f "$LOG" "$FIFO"
GDBOUT=$SRCDIR/build-gdb-sage040/sage040

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
make -s -C ../libc/test gdbee stracee ehtest ehtest.static >/dev/null 2>&1 || exit 1
make -s -C ../ports/bash >/dev/null 2>&1 || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
../ports/gdb/build.sh >/dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=48 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /usr; fsimg mkdir /usr/bin; fsimg mkdir /tmp
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
put_shells bash
for p in cat sleep kill true; do fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p; done
fsimg put -m 755 "$GDBOUT/bin/gdb" /usr/bin/gdb
fsimg put -m 755 "$GDBOUT/bin/gdbserver" /usr/bin/gdbserver
fsimg put -m 755 ../libc/test/gdbee /gdbee
fsimg put ../libc/test/gdbee.c /gdbee.c
fsimg put -m 755 ../libc/test/stracee /stracee
fsimg put -m 755 ../libc/test/ehtest /ehtest
fsimg put -m 755 ../libc/test/ehtest.static /ehtest.static
# Line numbers from the source, so an edit to gdbee.c cannot leave the
# sessions pointing at the wrong statement.
ln() { grep -n -- "$1" ../libc/test/gdbee.c | head -1 | cut -d: -f1; }
L_RESULT=$(ln 'int result = n \* n;')
L_LOOP=$(ln 'total += i;')
L_PRINTF=$(ln 'printf("%s: total')
L_HANDLED=$(ln 'printf("handled')
cat > "$SCRATCH/g1.gdb" <<GDB
set pagination off
set width 0
directory /
break square
run
bt
print n
print greeting
list $L_RESULT,$L_RESULT
print/x \$pc
up 1
print depth
frame 0
finish
break pick
continue
finish
break $L_HANDLED
continue
continue
print total
print p
continue
GDB
cat > "$SCRATCH/g2.gdb" <<GDB
set pagination off
directory /
break $L_LOOP
run
print i
next
next
print i
delete
tbreak $L_PRINTF
continue
print total
kill
GDB
fsimg put "$SCRATCH/g1.gdb" /g1.gdb
fsimg put "$SCRATCH/g2.gdb" /g2.gdb

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
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
    wait_for "$1-END [0-9]+" 3000 || echo "    (timed out: $1)"
}

sleep 5
# C++ exceptions first: gdb throws as ordinary control flow, so if these
# fail nothing below can work (libc/crtbegin-eh.s).
run EH "/ehtest"
run EHS "/ehtest.static"
# SHELL=/bin/sh: gdb and gdbserver start a program with `$SHELL -c
# exec PROG`, and the console's SHELL is msh, which is not a POSIX
# shell and has no exec.
# The sessions are command files (made on the host, below the disk
# setup): one command per line, and no shell between gdb and them.
run SESSION "/bin/bash -c 'SHELL=/bin/sh /usr/bin/gdb -batch -nx -x /g1.gdb /gdbee > /tmp/g1 2>&1'"
run SHOW1 "/bin/cat /tmp/g1"
run STEP "/bin/bash -c 'SHELL=/bin/sh /usr/bin/gdb -batch -nx -x /g2.gdb /gdbee > /tmp/g2 2>&1'"
run SHOW2 "/bin/cat /tmp/g2"
# -p: attach to a process already running, look, and let it go.
run ATTACH "/bin/bash -c '/stracee wait > /tmp/ticks & P=\$!; sleep 2; \
SHELL=/bin/sh /usr/bin/gdb -batch -nx -p \$P -ex \"info inferiors\" -ex \"print \\\$pc != 0\" -ex detach > /tmp/g3 2>&1; \
echo GDB-EXIT \$?; sleep 2; kill -0 \$P && echo STILL-RUNNING; kill \$P'"
run SHOW3 "/bin/cat /tmp/g3"
# gdbserver, driven over loopback.
run REMOTE "/bin/bash -c 'SHELL=/bin/sh /usr/bin/gdbserver 127.0.0.1:2345 /gdbee > /tmp/gs 2>&1 & S=\$!; sleep 3; \
SHELL=/bin/sh /usr/bin/gdb -batch -nx -ex \"directory /\" -ex \"target remote 127.0.0.1:2345\" -ex \"break square\" \
-ex continue -ex \"print n\" -ex bt -ex continue -ex continue /gdbee > /tmp/g4 2>&1; wait \$S; echo SERVER-EXIT \$?'"
run SHOW4 "/bin/cat /tmp/g4; /bin/cat /tmp/gs"
printf 'echo ALL-DONE\r' >&3
wait_for 'ALL-DONE' 300
printf 'halt\r' >&3
sleep 2
exec 3>&-; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
rm -f "$FIFO"
CLEAN="$SCRATCH/gdbtest-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[?0-9;]*[a-zA-Z]//g' > "$CLEAN"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
out() { awk -v a="$1" -v b="$2" '$0 ~ "^"b"-END" {on=0} on {print} $0 ~ "^"a"-END" {on=1}' "$CLEAN"; }
has() { echo "$1" | grep -aqE -- "$2"; }

echo "=== checks ==="
eh=$(tr -d '\r' < "$CLEAN" | awk '/\/ehtest; echo "EH"-END/ {on=1; next} /^EH-END/ {print; exit} on {print}')
grep -aqx 'EH-END 0' "$CLEAN" && echo "$eh" | grep -aqx 'EHTEST 3 ok 0 failed'
check "a C++ throw, three frames down, caught with its destructors run (dynamic)" $?
grep -aqx 'EHS-END 0' "$CLEAN" && [ "$(grep -acx 'EHTEST 3 ok 0 failed' "$CLEAN")" -eq 2 ]
check "  and in a static program" $?
g1=$(out SESSION SHOW1)
has "$g1" "^Breakpoint 1, square \\(n=7\\) at .*gdbee\\.c:$L_RESULT\$"
check "a breakpoint in square(), reached with its argument read off the stack (n=7)" $?
has "$g1" '^#0 +square \(n=7\)' && has "$g1" '^#1 +0x[0-9a-f]+ in nest \(depth=3\)' \
    && has "$g1" '^#2 +0x[0-9a-f]+ in nest \(depth=2\)' && has "$g1" '^#3 +0x[0-9a-f]+ in nest \(depth=1\)' \
    && has "$g1" '^#4 +0x[0-9a-f]+ in main \('
check "bt: square, nest x3 with their own depths, main -- the frame chain unwound" $?
has "$g1" '^\$1 = 7$'
check "print n: 7" $?
has "$g1" '^\$2 = 0x[0-9a-f]+ "hello from gdbee"$'
check "print greeting: a pointer, and the string read out of the tracee's memory" $?
has "$g1" "^$L_RESULT[[:space:]]+int result = n \\* n;\$"
check "list: the source line the breakpoint is on" $?
bp=$(echo "$g1" | sed -n 's/^Breakpoint 1 at \(0x[0-9a-f]*\): file .*/\1/p' | head -1)
[ -n "$bp" ] && has "$g1" "^\\\$3 = $bp\$"
check "\$pc is the breakpoint's own address ($bp): stepped back over the trap" $?
has "$g1" '^\$4 = 3$'
check "up 1, print depth: the caller's frame, 3" $?
has "$g1" '^Value returned is \$5 = 49$'
check "finish: square returns 49, out of d0" $?
has "$g1" '^Value returned is \$6 = 0x[0-9a-f]+ "hello from gdbee"$'
check "finish out of pick(): a pointer, read from d0 (this compiler's), and its string" $?
has "$g1" '^\$7 = 4950$' && has "$g1" '^\$8 = \{x = 12, y = -34\}$'
check "a second breakpoint: total 4950, and a struct, {x = 12, y = -34}" $?
has "$g1" '^Program received signal SIGUSR1, User defined signal 1\.$'
check "a signal stops the program and gdb names it (SIGUSR1)" $?
has "$g1" '^handled 1, point 12,-34$'
check "  continue passes it on, and the program's handler runs" $?
has "$g1" '^\[Inferior 1 \(process [0-9]+\) exited with code 05\]$'
check "the exit, with the program's status (5)" $?

g2=$(out STEP SHOW2)
has "$g2" '^\$1 = 0$' && has "$g2" '^\$2 = 1$'
check "next, twice: one more pass round the loop (i from 0 to 1)" $?
has "$g2" '^\$3 = 4950$'
check "tbreak after the loop: the whole loop ran, 4950" $?
! has "$g2" 'exited with code'
check "kill: the program does not run on to its exit" $?

g3=$(out ATTACH SHOW3)
grep -aqx 'GDB-EXIT 0' "$CLEAN" && has "$g3" '^\$1 = 1$' && has "$g3" '^\[Inferior 1 \(process [0-9]+\) detached\]$'
check "-p: attached to a running process, read its pc, detached" $?
grep -aqx 'STILL-RUNNING' "$CLEAN"
check "  and the process runs on after gdb lets it go" $?

g4=$(out REMOTE SHOW4)
has "$g4" "^Breakpoint 1, square \\(n=7\\) at .*gdbee\\.c:$L_RESULT\$" && has "$g4" '^\$1 = 7$' \
    && has "$g4" '^#3 +0x[0-9a-f]+ in nest \(depth=1\)'
check "gdbserver over loopback: the same breakpoint, value and backtrace" $?
has "$g4" '^Child exited with status 5$' && grep -aqx 'SERVER-EXIT 0' "$CLEAN"
check "  and gdbserver reports the exit and ends" $?

grep -aqE 'panic|bus error|address error|DOUBLE MMU FAULT' "$CLEAN"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
