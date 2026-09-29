#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# tlstest.sh - thread-local storage, and dlopen.
#
# libc/test/tlstest, twice: STATIC, where crt0 sets TLS up and dlopen
# must refuse, and DYNAMIC, linked against a library with TLS of its
# own and dlopening two more -- one it must load, TLS and dependency
# and constructor and all, and one it must refuse and leave no trace
# of. Each run's checks are its own lines; see tlstest.c for what each
# compares against.
#
# The kernel's part is small -- a thread pointer per task, get and
# set_thread_area, CLONE_SETTLS -- and is exercised by all of it: a
# thread that got its creator's pointer would pass nothing here.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-tls.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/tlstest.log"
FIFO="$SCRATCH/tls.fifo"
rm -f "$LOG"
BOOT_WAIT=${BOOT_WAIT:-4}
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

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
make -s -C ../ldso || exit 1
make -s -C ../libc/test tls || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /lib
fsimg put -m 755 ../ldso/ld.so /lib/ld.so
fsimg put -m 755 "$SAGE_LIBC/lib/libc.so" /lib/libc.so
for l in libtlsa libtlsb libtlsc libtlsie; do
    fsimg put -m 755 "../libc/test/$l.so" "/lib/$l.so"
done
fsimg put -m 755 ../libc/test/tlstest /tlstest
fsimg put -m 755 ../libc/test/tlstest.dyn /tlstest.dyn

rm -f "$FIFO"
mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$FIFO"

# A WHOLE LINE, as an extended regex: the command typed to produce a
# marker echoes it too, and a substring match returns on the echo --
# which sent the second program while the first was still running.
wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}

sleep "$BOOT_WAIT"
printf 'echo STATIC-START\r' >&3
printf '/tlstest; echo "STATIC-EXIT $?"\r' >&3
wait_for "STATIC-EXIT [0-9]+" 900
printf '/tlstest.dyn; echo "DYNAMIC-EXIT $?"\r' >&3
wait_for "DYNAMIC-EXIT [0-9]+" 900
sleep 0.3

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"

tr -d '\r' < "$LOG" > "$SCRATCH/tls-clean.tmp"
echo "=== guest session ==="
sed -n '/STATIC-START/,$p' "$SCRATCH/tls-clean.tmp" | sed 's/^/  | /'

# Each run's lines are the ones between its start and its exit status.
static=$(sed -n '/^STATIC-START/,/^STATIC-EXIT/p' "$SCRATCH/tls-clean.tmp")
dynamic=$(sed -n '/^STATIC-EXIT/,/^DYNAMIC-EXIT/p' "$SCRATCH/tls-clean.tmp")

for run in static dynamic; do
    echo "=== checks: $run ==="
    text=${!run}
    while IFS= read -r line; do
        case "$line" in
            "  ok   "*) check "$run: ${line#  ok   }" 0 ;;
            "  FAIL "*) check "$run: ${line#  FAIL }" 1 ;;
        esac
    done <<< "$text"
    grep -qE '^tlstest: [0-9]+ checks, 0 failed$' <<< "$text"
    check "$run: ran to the end, nothing failed" $?
done
grep -qx 'STATIC-EXIT 0' "$SCRATCH/tls-clean.tmp"
check "the static program exited 0" $?
grep -qx 'DYNAMIC-EXIT 0' "$SCRATCH/tls-clean.tmp"
check "the dynamic program exited 0" $?

grep -qE 'panic|bus error|address error|ld.so:' "$SCRATCH/tls-clean.tmp"
[ $? -ne 0 ]
check "no panic, no fault, no loader failure" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
