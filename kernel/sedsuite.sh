#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# sedsuite.sh - GNU sed's own test suite, on the machine.
#
# sedtest.sh checks the port with cases written here; this runs the
# tests sed ships with, as its Makefile would -- the same environment
# (TESTS_ENVIRONMENT, reproduced below), bash as the shell, sbase,
# diffutils, grep and awk for the tools they call -- and reads back the
# exit status of each: 0 pass, 77 skip, anything else fail. The .pl
# tests want Perl, which is not on this disk; they are skipped, here and
# in the count, saying so.
#
# Same shape as grepsuite.sh; see it for the comparison with the host.
#
# The verdicts are compared with the SAME tests run on the host against
# sed built natively from the same source (`make check` there): a test
# the host passes and the machine fails is a fault here, or a gap in
# what the machine has; one the host skips the machine may skip too. A
# test that fails on the machine is listed with the end of its log.
#
# SED_TESTS limits the run to some; SED_TIMEOUT changes the ten
# minutes each test is allowed.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-sedsuite.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/sedsuite.log"; FIFO="$SCRATCH/sedsuite.fifo"
WORK="$SCRATCH/sedsuite.tmp"
rm -rf "$LOG" "$WORK"; mkdir -p "$WORK"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "sedsuite: fsimg $* failed" >&2; exit 1; }; }
# For what may not be there -- a skipped test's log: no exit on failure.
fsimg_try() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@"; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
GSRC=$SRCDIR/sed-4.10
GBUILD=$SRCDIR/build-sed-sage040
HOSTBUILD=${SED_HOST_BUILD:-$SCRATCH/sed-host}

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
../ports/sed/build.sh >/dev/null || exit 1
../ports/grep/build.sh >/dev/null || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
make -s -C "$GBUILD" testsuite/get-mb-cur-max testsuite/test-mbrtowc >/dev/null 2>&1 || exit 1
TESTS=${SED_TESTS:-$(awk '/^T = /{f=1} f{print} f&&!/\\$/{exit}' "$GBUILD/Makefile" |
    tr -d '\\' | tr -s ' \t' '\n' | grep '^testsuite/' | tr '\n' ' ')}

# The host's verdicts: sed built natively from the same source, its
# own `make check`. Made once and kept (it takes a few minutes).
if [ ! -f "$HOSTBUILD/testsuite/misc.trs" ]; then
    echo "=== the host's run of the same suite ==="
    rm -rf "$HOSTBUILD"; mkdir -p "$HOSTBUILD"
    (cd "$HOSTBUILD" && "$GSRC/configure" --disable-nls >/dev/null 2>&1 &&
        make -j"$(nproc)" >/dev/null 2>&1 && make -j"$(nproc)" check >/dev/null 2>&1)
fi
host_result() { sed -n 's/^:test-result: //p' "$HOSTBUILD/${1%.*}.trs" 2>/dev/null | head -1; }

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=160 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /tmp /GT /GT/sed /GT/testsuite /usr /usr/bin /usr/lib /usr/lib/perl5; do fsimg mkdir $d; done
# Perl, for the tests written in it (misc.pl, debug.pl) and the ones
# that use it as a tool: the suite's Makefile says PERL=perl.
PERLOUT=$SRCDIR/build-perl-sage040/sage040
PV=$(ls "$PERLOUT/lib/perl5" | grep -E '^5\.[0-9]+\.[0-9]+$' | head -1)
fsimg put -m 755 "$PERLOUT/bin/perl" /usr/bin/perl
fsimg put -r "$PERLOUT/lib/perl5/$PV" "/usr/lib/perl5/$PV"
put_shells bash
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../ports/sbase/bin/* /bin/
for p in diff cmp; do fsimg put -m 755 "$SRCDIR/build-diffutils-sage040/sage040/bin/$p" /bin/$p; done
fsimg put -m 755 ../ports/sed/sed /bin/sed
fsimg put -m 755 ../ports/sed/sed /GT/sed/sed
fsimg put -m 755 ../ports/awk/awk /bin/awk
fsimg put -m 755 ../ports/grep/grep /bin/grep
for p in timeout locale; do fsimg put -m 755 ../utils/$p /bin/$p; done
fsimg put "$GSRC"/testsuite/* /GT/testsuite/
for h in get-mb-cur-max test-mbrtowc; do fsimg put -m 755 "$GBUILD/testsuite/$h" /GT/testsuite/$h; done
fsimg put "$GBUILD/config.h" /GT/config.h
# init.cfg is at the top of sed's tree, not in testsuite/: it is where
# print_ver_ and the require_* helpers live, and without it every test
# that should have skipped for want of valgrind ran valgrind instead.
fsimg put "$GSRC/init.cfg" /GT/init.cfg
for t in $TESTS; do fsimg_try put -m 755 "$GSRC/$t" "/GT/$t" >/dev/null 2>&1; done

# The runner: TESTS_ENVIRONMENT from sed's Makefile, by hand; from the
# top of the tree, as `make check` runs them.
# Each test under a ten-minute timeout, so one that hangs costs ten
# minutes and not the run.
# SED_TIMEOUT: each test's allowance, ten minutes unless said.
SED_TIMEOUT=${SED_TIMEOUT:-600}
cat > "$WORK/run.sh" <<EOF
cd /GT
export TMPDIR=/tmp VERSION=4.10 LC_ALL=C AWK=awk abs_top_builddir=/GT \\
    abs_top_srcdir=/GT abs_srcdir=/GT built_programs=sed \\
    srcdir=. top_srcdir=. CC=cc CONFIG_HEADER=/GT/config.h MAKE=make \\
    PACKAGE_BUGREPORT=bug-sed@gnu.org PACKAGE_VERSION=4.10 PERL=perl \\
    SHELL=/bin/bash PATH=/GT/sed:/bin:/usr/bin
. ./testsuite/envvar-check
: > /GT/results
for t in $TESTS; do
    # A .pl test runs as the Makefile's PL_LOG_COMPILER runs it.
    case \$t in
    *.pl) SED_TEST_NAME=\$(echo \$t | sed 's,/,-,g') timeout -k 10 $SED_TIMEOUT \\
              perl -w -I./testsuite -MCuSkip -MCoreutils \\
              -M"CuTmpdir qw(\$t)" ./\$t > \$t.log 2>&1 ;;
    *)    SED_TEST_NAME=\$(echo \$t | sed 's,/,-,g') timeout -k 10 $SED_TIMEOUT \\
              bash ./\$t > \$t.log 2>&1 ;;
    esac
    echo "\$t \$?" >> /GT/results
done
echo SEDSUITE-DONE
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

fsimg_try get /GT/results "$WORK/results" >/dev/null 2>&1
mkdir -p "$WORK/logs"
for t in $TESTS; do fsimg_try get "/GT/$t.log" "$WORK/logs/$(basename $t).log" >/dev/null 2>&1; done

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
    77) mskip=$((mskip + 1))
        # A skip the host does not take is a gap in what the machine
        # has (a tool, an option, Perl) -- not a failure, but said.
        if [ "$h" = PASS ]; then
            why=$(grep -aom1 'skipped test: .*' "$WORK/logs/$(basename $t).log" 2>/dev/null)
            echo "  [note] $t: skipped here, PASS on the host -- ${why:-no reason logged}"
        fi ;;
    *)  mfail=$((mfail + 1))
        if [ -n "${KNOWN[$t]:-}" ]; then
            echo "  [KNOWN] $t -- ${KNOWN[$t]}"; known=$((known + 1))
        elif [ "$h" = PASS ]; then
            echo "  [FAIL] $t: exit $st here, PASS on the host"
            tail -4 "$WORK/logs/$(basename $t).log" 2>/dev/null | sed 's/^/        /'
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
