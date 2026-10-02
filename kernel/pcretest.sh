#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# pcretest.sh - PCRE2's own tests on the machine, and grep -P.
#
# PCRE2 ships RunTest: two dozen files of patterns and subjects, each run
# through pcre2test and compared with the output it should give. It is
# run here exactly as upstream runs it -- bash, diff, the testdata
# directory -- with the library built as ports/pcre2 builds it (8-bit,
# Unicode, no JIT). The host runs the same RunTest against PCRE2 built
# natively with the same options, and every test the host passes must
# pass here. Then grep -P, which is what the library is for.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-pcre.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/pcretest.log"; FIFO="$SCRATCH/pcre.fifo"; WORK="$SCRATCH/pcre.tmp"
rm -rf "$LOG" "$WORK"; mkdir -p "$WORK"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "pcretest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
PSRC=$SRCDIR/pcre2-10.45
POUT=$SRCDIR/build-pcre2-sage040/sage040
HOSTBUILD=$SCRATCH/pcre2-host

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
../ports/pcre2/build.sh >/dev/null || exit 1
../ports/grep/build.sh >/dev/null || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
if [ ! -x "$HOSTBUILD/pcre2test" ]; then
    rm -rf "$HOSTBUILD"; mkdir -p "$HOSTBUILD"
    (cd "$HOSTBUILD" && "$PSRC/configure" --disable-shared --enable-static \
        --disable-jit --enable-unicode >/dev/null 2>&1 && make -j"$(nproc)" >/dev/null 2>&1)
fi
(cd "$HOSTBUILD" && srcdir="$PSRC" "$PSRC/RunTest" > "$WORK/host.out" 2>&1)
host_ok=$(grep -cE '^OK$|^  OK$|OK$' "$WORK/host.out")

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /tmp /P /P/testdata; do fsimg mkdir $d; done
put_shells bash
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../ports/sbase/bin/* /bin/
for p in diff cmp; do fsimg put -m 755 "$SRCDIR/build-diffutils-sage040/sage040/bin/$p" /bin/$p; done
fsimg put -m 755 ../ports/grep/grep /bin/grep
fsimg put -m 755 ../ports/sed/sed /bin/sed
fsimg put -m 755 "$POUT/bin/pcre2test" /P/pcre2test
fsimg put -m 755 "$PSRC/RunTest" /P/RunTest
fsimg put "$PSRC"/testdata/* /P/testdata/

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {
    local i
    for i in $(seq 1 "${2:-900}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_for "$1-END [0-9]+" "${3:-900}"; }
wait_for 'kernel ready.*' 300
sleep 0.5
run RUNTEST "/bin/bash -c 'cd /P && srcdir=. PATH=/bin ./RunTest > /P/run.out 2>&1'" 14400
run GP1 "echo 'foo123bar' | /bin/grep -oP '\\d+(?=bar)'"
# Through bash, for the LC_ALL= prefix the console's shell has not got.
run GP2 "/bin/bash -c \"printf 'caf\\\\303\\\\251\\\\n' | LC_ALL=C.UTF-8 /bin/grep -cP '^\\\\w{4}\\$'\""
run GP3 "echo 'aaa' | /bin/grep -P '(?<=a)a(?!a)'; echo GP3-RC=\$?"
printf 'sync\r' >&3; sleep 3
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"
PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" get /P/run.out "$WORK/machine.out" >/dev/null 2>&1
CL="$WORK/clean"; tr -d '\r' < "$LOG" > "$CL"
block() { sed -n "/echo \"$1\"-END/,/^$1-END/p" "$CL" | sed '1d;$d'; }

echo "=== checks ==="
[ -s "$WORK/machine.out" ]; check "RunTest ran on the machine" $?
grep -q 'FAILED\|failed' "$WORK/machine.out" && r=1 || r=0
check "  and nothing in it failed" $r
[ $r -ne 0 ] && grep -B2 -A6 -E 'FAILED|failed' "$WORK/machine.out" | head -20 | sed 's/^/        /'
# The tests the host ran, by name, each OK here too.
hn=$(grep -cE '^Test [0-9]+:' "$WORK/host.out"); mn=$(grep -cE '^Test [0-9]+:' "$WORK/machine.out")
[ "$hn" -gt 0 ] && [ "$mn" = "$hn" ]
check "the same $hn tests ran here as on the host ($mn)" $?
[ "$(block GP1)" = 123 ]; check "grep -oP with a lookahead: 123" $?
[ "$(block GP2)" = 1 ]; check "grep -P \\w in UTF-8 matches é (a 4-letter word)" $?
grep -qx 'GP3-RC=0' "$CL" && block GP3 | grep -qx aaa
check "grep -P lookbehind and negative lookahead" $?
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
