#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# greptest.sh - GNU grep, running on the machine (task 27).
#
# Two sets of tests:
#
#   tables   grep's own bre.tests, ere.tests and spencer1.tests: 432
#            patterns, each with an input line and the exit status
#            upstream says it must give (0 match, 1 none, 2 an invalid
#            pattern). One shell script on the machine runs them all,
#            each as `grep [-E] -f PATTERN INPUT`, and the statuses are
#            graded on the host against the table -- upstream's answers,
#            not a build's. Rows upstream marks as known non-conformance
#            are skipped, as its own test skips them.
#   ours     ports/grep/tests: the options, context, binary files, NUL
#            records, UTF-8 case folding, and -r over a directory tree
#            (openat and friends relative to an open directory). Each is
#            compared with what the same grep source prints when built
#            natively for the host.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-grep.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/greptest.log"
WORK="$SCRATCH/grep.tmp"
rm -f "$LOG"
rm -rf "$WORK/tests" "$WORK/expect" "$WORK/got" "$WORK/tables" "$WORK/hostrun"
BOOT_WAIT=${BOOT_WAIT:-4}

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
../ports/grep/build.sh >/dev/null || exit 1
VER=$(sed -n 's/^VERSION=//p' ../ports/grep/build.sh)
GREPSRC=${SAGE_SRC:-$HOME/m68k/src}/grep-$VER

if [ ! -x "$WORK/host/src/grep" ]; then
    mkdir -p "$WORK/host"
    (cd "$WORK/host" && "$GREPSRC/configure" --disable-nls -q >/dev/null &&
     make -s -j8 >/dev/null 2>&1) ||
        { echo "greptest: could not build grep for the host" >&2; exit 1; }
fi
HOSTGREP=$WORK/host/src/grep

# --- the tables: a pattern file and an input file per row ------------
mkdir -p "$WORK/tables"
: > "$WORK/tables.sh"
: > "$WORK/tables.want"
for tbl in bre ere spencer1; do
    flag=; [ "$tbl" != bre ] && flag=-E
    awk -F@ -v t="$tbl" -v flag="$flag" -v dir="$WORK/tables" -v sh="$WORK/tables.sh" \
        -v want="$WORK/tables.want" '
        /^#/ || NF != 3 { next }
        {
            n++; id = t n
            printf "%s\n", $2 > (dir "/" id ".p"); close(dir "/" id ".p")
            printf "%s\n", $3 > (dir "/" id ".i"); close(dir "/" id ".i")
            printf "grep %s -f %s.p %s.i > /dev/null 2>&1; echo %s $?\n", flag, id, id, id >> sh
            printf "%s %s\n", id, $1 >> want
        }' "$GREPSRC/tests/$tbl.tests"
done
NTAB=$(wc -l < "$WORK/tables.want")

# --- ours: what the host's grep says ---------------------------------
mkdir -p "$WORK/tests" "$WORK/expect" "$WORK/got"
cp -r ../ports/grep/tests/. "$WORK/tests/"
TESTS=$(cd ../ports/grep/tests && ls *.args | sed 's/\.args$//')
export LANG=C.UTF-8
cp -r "$WORK/tests" "$WORK/hostrun"
for t in $TESTS; do
    # shellcheck disable=SC2046
    (cd "$WORK/hostrun" && set -f && PATH="$(dirname "$HOSTGREP"):$PATH" \
        grep $(cat "$t.args") > "$WORK/expect/$t.OUT" 2>&1
     echo "status $?" >> "$WORK/expect/$t.OUT")
done

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /GREPT; fsimg mkdir /GTAB
fsimg put -m 755 ../system/sh /bin/sh
fsimg put -m 755 ../ports/grep/grep /bin/grep
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put "$WORK"/tests/* /GREPT/
fsimg put "$WORK"/tables/* /GTAB/
fsimg put "$WORK/tables.sh" /GTAB/TABLES.SH

rm -f "$SCRATCH/grep.fifo"
mkfifo "$SCRATCH/grep.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/grep.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/grep.fifo"

wait_for() {                    # wait_for LINE [TRIES]
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxF -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2" "${3:-600}"
    sleep 0.1
}

sleep "$BOOT_WAIT"
run 'export LANG=C.UTF-8' lang
run 'cd /GTAB' cd1
run 'sh TABLES.SH > TABLES.OUT' tables 6000
run 'cd /GREPT' cd2
n=0
for t in $TESTS; do
    n=$((n + 1))
    run "grep $(cat "../ports/grep/tests/$t.args") > $t.OUT 2>&1; echo status \$? >> $t.OUT" "t$n"
done
run 'halt' halt

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/grep.fifo"

fsimg get -r /GREPT "$WORK/got" 2>/dev/null; mv "$WORK/got/GREPT"/* "$WORK/got/" 2>/dev/null
fsimg get /GTAB/TABLES.OUT $WORK/tables.got 2>/dev/null
tr -d '\r' < "$LOG" > "$SCRATCH/grep-clean.tmp"
tr -d '\r' < "$WORK/tables.got" > "$WORK/tables.got2" 2>/dev/null

echo "=== guest session (last 8 lines) ==="
tail -8 "$SCRATCH/grep-clean.tmp" | sed 's/^/  | /'

echo "=== checks: grep's own pattern tables (bre, ere, spencer1) ==="
for tbl in bre ere spencer1; do
    total=$(grep -c "^$tbl[0-9]* " "$WORK/tables.want")
    bad=$(join <(grep "^$tbl[0-9]* " "$WORK/tables.want" | sort) \
               <(grep "^$tbl[0-9]* " "$WORK/tables.got2" | sort) |
          awk '$2 != $3 { print $1 " wanted " $2 " got " $3 }')
    got=$(grep -c "^$tbl[0-9]* " "$WORK/tables.got2")
    [ -z "$bad" ] && [ "$got" -eq "$total" ]
    check "$tbl: $got of $total cases ran, every exit status as upstream says" $?
    [ -n "$bad" ] && echo "$bad" | head -8 | sed 's/^/        /'
done

echo "=== checks: ours, against the same grep built for the host ==="
for t in $TESTS; do
    if [ -f "$WORK/tests/$t.sort" ]; then
        cmp -s <(sort "$WORK/got/$t.OUT" 2>/dev/null) <(sort "$WORK/expect/$t.OUT")
    else
        cmp -s "$WORK/got/$t.OUT" "$WORK/expect/$t.OUT"
    fi
    r=$?
    check "$t: grep $(cat "$WORK/tests/$t.args")" $r
    [ $r -ne 0 ] && diff "$WORK/expect/$t.OUT" "$WORK/got/$t.OUT" 2>&1 | head -8 | sed 's/^/        /'
done
# What was agreed on has to be real.
grep -q "status 2" "$WORK/expect/badre.OUT" && grep -q "status 1" "$WORK/expect/nomatch.OUT" &&
    grep -qx "tree/sub/deeper/c.txt:deep needle" "$WORK/expect/recurse.OUT" &&
    grep -q "École" "$WORK/expect/utf8i.OUT"
check "the host's answers are real: errors, no-match, a deep -r hit, a UTF-8 case fold" $?

! grep -q "panic\|exception" "$SCRATCH/grep-clean.tmp"
check "no panic, no kernel exception" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
