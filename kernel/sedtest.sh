#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# sedtest.sh - GNU sed, running on the machine (task 26).
#
# Each test in ports/sed/tests is run inside the guest as
#     sed FLAGS -f T.sed FILES > T.OUT 2>&1
# FLAGS from T.flags -- before -f, because sed compiles a -f script as
# soon as it reads it, so a -E after it comes too late -- and FILES from
# T.args, or T.in if there is none,
# and every T.OUT is read back off the disk and compared ON THE HOST with
# what the SAME sed source prints when built natively for the host -- an
# independent run of the same program, not the machine agreeing with
# itself. Files the scripts write (w) are compared the same way, and
# `sed -i` is checked by reading the edited file back from the image.
#
# sed's own test suite is shell scripts over coreutils and a POSIX sh;
# it runs once bash and the utilities exist (tasks 28 and 29).

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-sed.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/sedtest.log"
WORK="$SCRATCH/sed.tmp"
rm -f "$LOG"
rm -rf "$WORK/tests" "$WORK/expect" "$WORK/got"
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
../ports/sed/build.sh >/dev/null || exit 1
VER=$(sed -n 's/^VERSION=//p' ../ports/sed/build.sh)
SEDSRC=${SAGE_SRC:-$HOME/m68k/src}/sed-$VER

# The same source, for this host, once: configure takes a minute.
if [ ! -x "$WORK/host/sed/sed" ]; then
    mkdir -p "$WORK/host"
    (cd "$WORK/host" && "$SEDSRC/configure" --disable-nls -q >/dev/null &&
     make -s -j8 sed/sed >/dev/null 2>&1) ||
        { echo "sedtest: could not build sed for the host" >&2; exit 1; }
fi
HOSTSED=$WORK/host/sed/sed

mkdir -p "$WORK/tests" "$WORK/expect" "$WORK/got"
cp ../ports/sed/tests/* "$WORK/tests/"
TESTS=$(cd ../ports/sed/tests && ls *.sed | sed 's/\.sed$//')
flags_of() {
    [ -f "$WORK/tests/$1.flags" ] && cat "$WORK/tests/$1.flags"
}
args_of() {
    if [ -f "$WORK/tests/$1.args" ]; then cat "$WORK/tests/$1.args"
    elif [ -f "$WORK/tests/$1.in" ]; then echo "$1.in"
    fi
}

# What each should print, and write, in the same locale on both sides.
export LANG=C.UTF-8
mkdir -p "$WORK/hostrun"
cp "$WORK"/tests/* "$WORK/hostrun/"
for t in $TESTS; do
    # shellcheck disable=SC2046
    (cd "$WORK/hostrun" && PATH="$(dirname "$HOSTSED"):$PATH" sed $(flags_of "$t") -f "$t.sed" $(args_of "$t") \
        > "$WORK/expect/$t.OUT" 2>&1; echo "status $?" >> "$WORK/expect/$t.OUT")
done
(cd "$WORK/hostrun" && "$HOSTSED" -i 's/change/CHANGED/' inplace.dat)

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /SEDT
fsimg put -m 755 ../system/sh /bin/sh
fsimg put -m 755 ../ports/sed/sed /bin/sed
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put "$WORK"/tests/* /SEDT/

rm -f "$SCRATCH/sed.fifo"
mkfifo "$SCRATCH/sed.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/sed.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/sed.fifo"

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
run 'cd /SEDT' cd
run 'export LANG=C.UTF-8' lang
n=0
for t in $TESTS; do
    n=$((n + 1))
    run "sed $(flags_of "$t") -f $t.sed $(args_of "$t") > $t.OUT 2>&1; echo status \$? >> $t.OUT" "t$n"
done
run "sed -i s/change/CHANGED/ inplace.dat" inplace
run 'halt' halt

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/sed.fifo"

fsimg get -r /SEDT "$WORK/got" 2>/dev/null; mv "$WORK/got/SEDT"/* "$WORK/got/" 2>/dev/null
tr -d '\r' < "$LOG" > "$SCRATCH/sed-clean.tmp"

echo "=== guest session (last 12 lines) ==="
tail -12 "$SCRATCH/sed-clean.tmp" | sed 's/^/  | /'

echo "=== checks: sed on the machine, against the same sed built for the host ==="
for t in $TESTS; do
    cmp -s "$WORK/got/$t.OUT" "$WORK/expect/$t.OUT"
    r=$?
    check "$t" $r
    [ $r -ne 0 ] && diff "$WORK/expect/$t.OUT" "$WORK/got/$t.OUT" 2>&1 | head -8 | sed 's/^/        /'
done
cmp -s "$WORK/got/files.w" "$WORK/hostrun/files.w"
check "  and the file 'w' wrote" $?
cmp -s "$WORK/got/inplace.dat" "$WORK/hostrun/inplace.dat"
check "sed -i edited the file in place" $?
grep -q "CHANGED" "$WORK/got/inplace.dat"
check "  (and the edit is really there, not two unedited copies agreeing)" $?
grep -q "status 5" "$WORK/expect/tt.OUT"
check "the host's own run produced the answers compared with (q5 exits 5)" $?
# Agreement is only worth something if what was agreed on is real: two
# runs failing the same way would match too.
bad=
for t in $TESTS; do
    [ "$t" = tt ] && continue
    # -c, not -l: zero's records end in NUL, not newline.
    { [ "$(wc -c < "$WORK/expect/$t.OUT")" -gt 12 ] && grep -qa "status 0$" "$WORK/expect/$t.OUT"; } ||
        bad="$bad $t"
done
[ -z "$bad" ]
check "  and every other test printed something and exited 0 there${bad:+ (not:$bad)}" $?
grep -q "WÖRLD" "$WORK/expect/utf8.OUT"
check "  the UTF-8 test really upper-cased a non-ASCII letter" $?
! grep -q "panic\|exception" "$SCRATCH/sed-clean.tmp"
check "no panic, no kernel exception" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
