#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# bashtest.sh - GNU bash, running on the machine (task 28).
#
# Two sets:
#
#   lang.sh   the shell language -- expansion, arithmetic, [[ ]], arrays,
#             functions, loops, pipes, here-documents, traps, jobs,
#             getopts -- run by bash on the machine and by bash on the
#             host, and the two compared. Nothing in it may depend on
#             where it runs.
#   suite     a subset of bash's own test suite (tests/), run on the
#             machine: each NAME.tests is run by the shell and its
#             output compared, ON THE HOST, with the NAME.right upstream
#             ships. A subset because one test takes minutes at 25 MHz;
#             BASH_TESTS names others, and `make bashsuite` runs all 83
#             (hours).
#
# sbase's utilities are on the disk: the suite uses them throughout.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-bash.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/bashtest.log"
WORK="$SCRATCH/bash.tmp"
rm -f "$LOG"
rm -rf "$WORK"
mkdir -p "$WORK/got"
BOOT_WAIT=${BOOT_WAIT:-4}
DISK_MB=${BASH_DISK_MB:-32}
# Which of bash's own tests to run. One takes minutes on a 25 MHz 68040,
# so this is a subset that covers the language; `make bashsuite` runs
# every one of the 83.
BASH_TESTS=${BASH_TESTS:-"arith array braces case comsub func glob quote strip type varenv"}

pass=0
fail=0
check() {
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
../ports/bash/build.sh >/dev/null || exit 1
../ports/sbase/build.sh >/dev/null || exit 1
# The four helper programs its own suite uses.
BASHBUILD=${SAGE_SRC:-$HOME/m68k/src}/build-bash-sage040
BASHSRC=${SAGE_SRC:-$HOME/m68k/src}/bash-5.3
# BASH_TESTS=all: every one of them, which takes hours.
if [ "$BASH_TESTS" = all ]; then
    BASH_TESTS=$(cd "$BASHSRC/tests" && ls *.tests | sed 's/\.tests$//' | tr '\n' ' ')
fi
make -s -C "$BASHBUILD" recho zecho printenv xcase >/dev/null 2>&1 || true

# What the host's bash makes of the same script.
(cd ../ports/bash/tests && bash lang.sh > "$WORK/lang.expect" 2>&1)

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count="$DISK_MB" status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /BT; fsimg mkdir /tmp; fsimg mkdir /share; fsimg mkdir /share/misc
fsimg put -m 755 ../system/sh /bin/sh
fsimg put -m 755 ../ports/bash/bash /bin/bash
fsimg put -m 755 ../ports/sbase/bin/* /bin/
fsimg put -m 755 ../ports/sbase/share/misc/bc.library /share/misc/
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../ports/bash/tests/lang.sh /BT/
# bash's own test suite, and the helpers it runs.
fsimg mkdir /BT/tests
fsimg put "$BASHSRC"/tests/* /BT/tests/ 2>/dev/null
fsimg put -m 755 ../ports/bash/tests/runsuite.sh /BT/tests/
# Its own runners invoke the shell as ./bash as well as $THIS_SH.
fsimg put -m 755 ../ports/bash/bash /BT/tests/bash

#
# THE TEST HELPERS, BUILT FOR THE MACHINE.
#
# bash's suite shells out to four little programs of its own -- recho,
# zecho, printenv and xcase -- and to `grep`. The build directory's
# copies of the four are built by the HOST compiler for the host, so
# copying them there put x86-64 binaries on an m68k disk and every test
# that used one answered "cannot execute binary file: Exec format
# error". They come from support/*.c and are cross-compiled here.
#
# grep is GNU grep from ports/, because sbase has none, and without it
# array.tests' ignore_builtin_arrays() -- a bare `grep -v` -- silently
# drops the output it was meant to filter.
#
# Seven of bash's own cases were failing on these two things, and had
# been failing on them for as long as the suite has existed.
#
HELPERS=$WORK/helpers
mkdir -p "$HELPERS"
# In a SUBSHELL: ports/cross.sh exits when picolibc is not built, and
# sourcing it here would take the suite down with it.
(
    TOP=$(cd .. && pwd)
    . "$TOP/ports/cross.sh"
    for h in recho zecho printenv xcase; do
        [ -f "$BASHSRC/support/$h.c" ] || continue
        "$CROSS_CC" $CROSS_CPPFLAGS $SPECS_CFLAGS -O2 -w \
            "$BASHSRC/support/$h.c" -o "$HELPERS/$h"
    done
) >/dev/null 2>&1
for h in recho zecho printenv xcase; do
    [ -f "$HELPERS/$h" ] && fsimg put -m 755 "$HELPERS/$h" /BT/tests/$h
done
# The utilities bash's own tests shell out to. sbase has none of these
# three, and without them the cases that use them do not fail loudly --
# they produce output with a "command not found" where the filtered
# text should be, which then differs from upstream's .right file in a
# way that reads as a fault in the shell.
for u in grep sed awk; do
    [ -x "../ports/$u/$u" ] && fsimg put -m 755 "../ports/$u/$u" /bin/$u
done

rm -f "$SCRATCH/bash.fifo"
mkfifo "$SCRATCH/bash.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/bash.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/bash.fifo"

wait_for() {
    for _ in $(seq 1 "${2:-900}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxF -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2" "${3:-900}"
    sleep 0.1
}

sleep "$BOOT_WAIT"
run 'cd /BT' cd
run 'bash --version > version.out' version
run 'bash lang.sh > lang.out 2>&1; echo status $? >> lang.out' lang 1800
# Upstream's suite: one bash loop over every NAME.tests, so that the
# machine is not asked 83 times over a serial line. Its own runners do
# the same thing with diff, which this system has none of yet -- the
# outputs are compared here.
run 'cd /BT/tests' cdtests
# What its own runners set: the shell under test, by name and by path.
run 'export PATH=.:/bin' pathset
run 'export TMPDIR=/tmp' tmpset
run 'export THIS_SH=/BT/tests/bash' thissh
run 'export BASH=/BT/tests/bash' bashvar
run "export BASH_TESTS='$BASH_TESTS'" whichtests
run 'bash runsuite.sh > runsuite.out 2>&1' suite 20000
run 'cd /BT' cdback
run 'halt' halt

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/bash.fifo"
fsimg get -r /BT "$WORK/got" 2>/dev/null; mv "$WORK/got/BT"/*.out "$WORK/got/" 2>/dev/null
mkdir -p "$WORK/suite"
fsimg get -r /BT/tests "$WORK/suite" 2>/dev/null; mv "$WORK/suite/tests"/*.out "$WORK/suite/" 2>/dev/null
tr -d '\r' < "$LOG" > "$SCRATCH/bash-clean.tmp"

echo "=== guest session (last 6 lines) ==="
tail -6 "$SCRATCH/bash-clean.tmp" | sed 's/^/  | /'

echo "=== checks ==="
grep -q "GNU bash, version 5.3.20" "$WORK/got/version.out"
check "bash 5.3.20 runs: $(head -1 "$WORK/got/version.out" 2>/dev/null)" $?
sed '$d' "$WORK/got/lang.out" > "$WORK/lang.got"    # drop the status line
cmp -s "$WORK/lang.got" "$WORK/lang.expect"
r=$?
check "the language, line for line as the host's bash has it" $r
[ $r -ne 0 ] && diff "$WORK/lang.expect" "$WORK/lang.got" | head -20 | sed 's/^/        /'
grep -qx "status 0" "$WORK/got/lang.out"
check "  and it ran to the end" $?
grep -qx "== done" "$WORK/lang.expect"
check "  (the host's own run got there too)" $?
echo "=== checks: bash's own test suite ==="
ran=0
for n in $BASH_TESTS; do
    [ -f "$WORK/suite/$n.out" ] || continue
    ran=$((ran + 1))
    cmp -s "$WORK/suite/$n.out" "$BASHSRC/tests/$n.right"
    r=$?
    check "$n" $r
    [ $r -ne 0 ] && diff "$BASHSRC/tests/$n.right" "$WORK/suite/$n.out" 2>&1 | head -6 |
        sed 's/^/        /'
done
[ "$ran" -eq "$(echo $BASH_TESTS | wc -w)" ]
check "every test asked for ran ($ran of $(echo $BASH_TESTS | wc -w))" $?

! grep -q "panic\|exception" "$SCRATCH/bash-clean.tmp"
check "no panic, no kernel exception" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
