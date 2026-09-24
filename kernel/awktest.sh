#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# awktest.sh - the one true awk, running on the machine (task 25).
#
# Two sets of tests, each run inside the guest as
#     awk -f T.awk [T.in | the words in T.args] > T.OUT 2>&1
# with every T.OUT read back off the disk and compared ON THE HOST:
#
#   upstream   awk's own bugs-fixed/ regression tests, against their
#              .ok (or .ok2) files. Their messages name the binary
#              "../a.out"; here it is "awk", which is the one change
#              made to what they expect.
#   ours       ports/awk/tests: fields, numbers and printf, strings,
#              regexes, arrays, pipes, files, getline, functions. What
#              they should print is worked out by the SAME awk source
#              built natively on the host -- an independent run of the
#              same program, not this machine agreeing with itself.
#
# awk is linked against /lib/libc.so, so the disk gets ld.so and
# libc.so, and /bin/sh, which awk's system() and pipes run commands
# through.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-awk.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/awktest.log"
WORK="$SCRATCH/awk.tmp"
rm -f "$LOG"
rm -rf "$WORK"
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
../ports/awk/build.sh >/dev/null || exit 1
AWKSRC=$(sed -n 's/^VERSION=//p' ../ports/awk/build.sh)
AWKSRC=${SAGE_SRC:-$HOME/m68k/src}/original-awk-$AWKSRC

# The same source, for this host, in a directory of its own.
mkdir -p "$WORK/host" "$WORK/tests"
cc -O2 -w -o "$WORK/host/awk" "$AWKSRC"/{b,main,parse,proctab,tran,lib,run,lex,awkgram.tab}.c -lm \
    || { echo "awktest: could not build awk for the host" >&2; exit 1; }

# The tests: upstream's and ours, in one directory, and what each
# should print.
cp "$AWKSRC"/bugs-fixed/*.awk "$AWKSRC"/bugs-fixed/*.in "$WORK/tests/" 2>/dev/null
UPSTREAM=$(cd "$AWKSRC/bugs-fixed" && ls *.awk | sed 's/\.awk$//')
# Tests that need a tool this system does not have yet, and which task
# brings it. Reported as SKIP, not passed; remove each as it lands.
declare -A NEEDS=(
    [space]="sort, and LC_ALL=C cmd in the shell (tasks 29 and 28)"
    [system-status]="kill -SIGNAME and \$\$ in the shell (task 28)"
)
cp ../ports/awk/tests/* "$WORK/tests/"
OURS=$(cd ../ports/awk/tests && ls *.awk | sed 's/\.awk$//')
mkdir -p "$WORK/expect"
for t in $UPSTREAM; do
    for k in ok ok2; do
        [ -f "$AWKSRC/bugs-fixed/$t.$k" ] &&
            sed 's|\.\./a\.out|awk|g' "$AWKSRC/bugs-fixed/$t.$k" > "$WORK/expect/$t.$k"
    done
done
args_of() {                     # args_of TEST: the input files it takes
    if [ -f "$WORK/tests/$1.args" ]; then cat "$WORK/tests/$1.args"
    elif [ -f "$WORK/tests/$1.in" ]; then echo "$1.in"
    fi
}
# Both sides in the same locale: upstream's tests expect UTF-8, and
# multibyte-aware awk is what a UTF-8 locale gets.
export LANG=C.UTF-8
for t in $OURS; do
    # shellcheck disable=SC2046
    (cd "$WORK/tests" && PATH="$WORK/host:$PATH" awk -f "$t.awk" $(args_of "$t") \
        > "$WORK/expect/$t.ok" 2>&1; true)
done

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /AWKT
for p in sh rm echo; do
    [ -f "../system/$p" ] && fsimg put -m 755 "../system/$p" "/bin/$p"
done
fsimg put -m 755 ../ports/awk/awk /bin/awk
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put "$WORK"/tests/* /AWKT/

rm -f "$SCRATCH/awk.fifo"
mkfifo "$SCRATCH/awk.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/awk.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/awk.fifo"

wait_for() {
    for _ in $(seq 1 600); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxF -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2"
    sleep 0.1
}

sleep "$BOOT_WAIT"
run 'cd /AWKT' cd
run 'export LANG=C.UTF-8' lang
n=0
for t in $UPSTREAM $OURS; do
    n=$((n + 1))
    [ -n "${NEEDS[$t]:-}" ] && continue
    # shellcheck disable=SC2046
    run "awk -f $t.awk $(args_of "$t") > $t.OUT 2>&1" "t$n"
done
run 'halt' halt 2>/dev/null || true

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/awk.fifo"

mkdir -p "$WORK/got"
fsimg get -r /AWKT "$WORK/got" 2>/dev/null; mv "$WORK/got/AWKT"/* "$WORK/got/" 2>/dev/null
tr -d '\r' < "$LOG" > "$SCRATCH/awk-clean.tmp"

echo "=== guest session (last 20 lines) ==="
tail -20 "$SCRATCH/awk-clean.tmp" | sed 's/^/  | /'

grade() {                       # grade TEST
    local got="$WORK/got/$1.OUT"
    [ -f "$got" ] || return 1
    cmp -s "$got" "$WORK/expect/$1.ok" && return 0
    [ -f "$WORK/expect/$1.ok2" ] && cmp -s "$got" "$WORK/expect/$1.ok2"
}

echo "=== checks: awk's own regression tests (bugs-fixed) ==="
for t in $UPSTREAM; do
    if [ -n "${NEEDS[$t]:-}" ]; then
        echo "  [SKIP] $t -- needs ${NEEDS[$t]}"
        continue
    fi
    grade "$t"
    r=$?
    check "$t" $r
    if [ $r -ne 0 ] && [ -f "$WORK/got/$t.OUT" ]; then
        diff "$WORK/expect/$t.ok" "$WORK/got/$t.OUT" | head -6 | sed 's/^/        /'
    fi
done

echo "=== checks: ours, against the same awk built for the host ==="
for t in $OURS; do
    grade "$t"
    r=$?
    check "$t" $r
    if [ $r -ne 0 ] && [ -f "$WORK/got/$t.OUT" ]; then
        diff "$WORK/expect/$t.ok" "$WORK/got/$t.OUT" | head -8 | sed 's/^/        /'
    fi
done
test "$(wc -l < "$WORK/expect/big.ok")" -gt 3
check "the host's own run of the tests produced something to compare with" $?

! grep -q "panic\|exception" "$SCRATCH/awk-clean.tmp"
check "no panic, no kernel exception" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
