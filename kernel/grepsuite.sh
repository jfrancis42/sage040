#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# grepsuite.sh - GNU grep's own test suite, on the machine.
#
# greptest.sh checks the port with cases written here; this runs the
# 128 tests grep ships with, each as its Makefile would -- the same
# environment (TESTS_ENVIRONMENT, reproduced below), bash as the shell,
# sbase, diffutils, sed and awk for the tools they call -- and reads
# back the exit status of each: 0 pass, 77 skip, anything else fail.
#
# The verdicts are compared with the SAME tests run on the host against
# grep built natively from the same source (`make check` there): a test
# the host passes and the machine fails is a fault here, or a gap in
# what the machine has; one the host skips the machine may skip too. A
# test that fails on the machine is listed with the end of its log.
#
# GREP_TESTS limits the run to some; GREP_KNOWN lists failures that are
# understood (each with its reason below), reported [KNOWN].

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-grepsuite.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/grepsuite.log"; FIFO="$SCRATCH/grepsuite.fifo"
WORK="$SCRATCH/grepsuite.tmp"
rm -rf "$LOG" "$WORK"; mkdir -p "$WORK"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "grepsuite: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
GSRC=$SRCDIR/grep-3.12
GBUILD=$SRCDIR/build-grep-sage040
HOSTBUILD=${GREP_HOST_BUILD:-$SCRATCH/grep-host}

pass=0; fail=0; known=0; skip=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../utils >/dev/null || exit 1
../ports/grep/build.sh >/dev/null || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
make -s -C "$GBUILD/tests" get-mb-cur-max >/dev/null 2>&1 || exit 1
TESTS=${GREP_TESTS:-$(awk '/^TESTS = /{f=1} f{print} f&&!/\\$/{exit}' "$GBUILD/tests/Makefile" |
    tr -d '\\' | tr -s ' \t' '\n' | grep -v '^TESTS$\|^=$\|^$' | tr '\n' ' ')}

# The host's verdicts: grep built natively from the same source, its
# own `make check`. Made once and kept (it takes a few minutes).
if [ ! -f "$HOSTBUILD/tests/backref.trs" ]; then
    echo "=== the host's run of the same suite ==="
    rm -rf "$HOSTBUILD"; mkdir -p "$HOSTBUILD"
    (cd "$HOSTBUILD" && "$GSRC/configure" --disable-nls >/dev/null 2>&1 &&
        make -j"$(nproc)" >/dev/null 2>&1 && make -j"$(nproc)" check >/dev/null 2>&1)
fi
host_result() { sed -n 's/^:test-result: //p' "$HOSTBUILD/tests/$1.trs" 2>/dev/null | head -1; }

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /tmp /GT /GT/src /GT/tests; do fsimg mkdir $d; done
put_shells bash
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../ports/sbase/bin/* /bin/
for p in diff cmp; do fsimg put -m 755 "$SRCDIR/build-diffutils-sage040/sage040/bin/$p" /bin/$p; done
fsimg put -m 755 ../ports/sed/sed /bin/sed
fsimg put -m 755 ../ports/awk/awk /bin/awk
fsimg put -m 755 ../ports/grep/grep /bin/grep
fsimg put -m 755 ../ports/grep/grep /GT/src/grep
for p in timeout locale; do fsimg put -m 755 ../utils/$p /bin/$p; done
fsimg put "$GSRC"/tests/* /GT/tests/ 2>/dev/null
fsimg put -m 755 "$GBUILD/tests/get-mb-cur-max" /GT/tests/get-mb-cur-max
fsimg put "$GBUILD/config.h" /GT/config.h
for t in $TESTS; do fsimg put -m 755 "$GSRC/tests/$t" "/GT/tests/$t" >/dev/null 2>&1; done

# The runner: TESTS_ENVIRONMENT from grep's tests/Makefile, by hand.
# Each test under a ten-minute timeout, so one that hangs costs ten
# minutes and not the run.
cat > "$WORK/run.sh" <<EOF
cd /GT/tests
export TMPDIR=/tmp VERSION=3.12 LC_ALL=C AWK=awk abs_top_builddir=/GT \\
    abs_top_srcdir=/GT abs_srcdir=/GT/tests built_programs=grep \\
    host_triplet=m68k-unknown-elf srcdir=. top_srcdir=.. CC=cc \\
    CONFIG_HEADER=/GT/config.h MAKE=make PACKAGE_BUGREPORT=bug-grep@gnu.org \\
    PACKAGE_VERSION=3.12 PERL=perl SHELL=/bin/bash PATH=/GT/src:/bin \\
    PCRE_WORKS=0
. ./envvar-check
: > /GT/results
for t in $TESTS; do
    GREP_TEST_NAME=\$t timeout -k 10 600 bash ./\$t > \$t.log 2>&1
    echo "\$t \$?" >> /GT/results
done
echo GREPSUITE-DONE
EOF
fsimg put -m 755 "$WORK/run.sh" /GT/run.sh

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
        sleep 1
    done
    return 1
}
wait_for 'kernel ready.*' 300
sleep 1
printf '/bin/bash /GT/run.sh > /GT/run.out 2>&1; echo "SUITE-END $?"\r' >&3
wait_for 'SUITE-END [0-9]+' 30000 || echo "    (the run did not finish)"
printf 'sync; halt\r' >&3
sleep 5
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

fsimg get /GT/results "$WORK/results" >/dev/null 2>&1
mkdir -p "$WORK/logs"
for t in $TESTS; do fsimg get "/GT/tests/$t.log" "$WORK/logs/$t.log" >/dev/null 2>&1; done

# Failures understood, and why. Keep the reason honest: "not a bug in
# the kernel" has to be shown, not assumed.
declare -A KNOWN=()

echo "=== checks ==="
[ -s "$WORK/results" ]; check "the suite ran and left its results" $?
mpass=0; mskip=0; mfail=0; worse=""
while read -r t st; do
    h=$(host_result "$t")
    case "$st" in
    0)  mpass=$((mpass + 1)) ;;
    77) mskip=$((mskip + 1)) ;;
    *)  mfail=$((mfail + 1))
        if [ -n "${KNOWN[$t]:-}" ]; then
            echo "  [KNOWN] $t -- ${KNOWN[$t]}"; known=$((known + 1))
        elif [ "$h" = PASS ]; then
            echo "  [FAIL] $t: exit $st here, PASS on the host"
            tail -4 "$WORK/logs/$t.log" 2>/dev/null | sed 's/^/        /'
            fail=$((fail + 1)); worse="$worse $t"
        else
            echo "  [SKIP] $t: exit $st here, ${h:-no verdict} on the host too"
            skip=$((skip + 1))
        fi ;;
    esac
done < "$WORK/results"
n=$(wc -l < "$WORK/results" 2>/dev/null || echo 0); nt=$(echo $TESTS | wc -w)
[ "$n" = "$nt" ]; check "every test ran ($n of $nt)" $?
echo "  on the machine: $mpass passed, $mskip skipped, $mfail failed"
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$LOG"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
echo "  known:  $known"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
