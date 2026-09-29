#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# fpsptest.sh - Motorola's M68040 FPSP in the kernel, and the maths it does.
#
# The 68040 has no silicon for FSIN, FETOX, FLOGN and the rest of the
# MC68881's transcendentals; on the real chip they trap, and the FPSP in
# the kernel (kernel/fpsp/) computes them. QEMU normally computes them
# itself, so this suite boots the machine TWICE:
#
#   -cpu m68040                   QEMU's softfloat does the maths, and
#                                 the kernel must never be asked
#   -cpu m68040,fpsp-trap=on      every one of those instructions traps,
#                                 as on the chip, and the FPSP does it
#
# and checks both against a third implementation: the host's libm,
# whose answers apps/fpsptest carries. The kernel's count of emulated
# instructions (KSTAT_FPSP) says which of the two actually did the work,
# so a pass that the CPU earned cannot be mistaken for the FPSP's.
#
# The trapping boot also runs two copies at once, so that tasks switch
# while the package is in the middle of an instruction, and runs an
# F-line instruction that is not floating point, which the package must
# hand back as SIGILL.
#
# Not tested here, because QEMU raises no floating-point arithmetic
# exception (overflow, divide by zero, ...) whatever FPCR enables: the
# package's paths that report those as SIGFPE.
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
DISK="$SCRATCH/hd-fpsp.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/fpsptest.log"
FIFO="$SCRATCH/fpsp.fifo"
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
make -s -C ../apps fpsptest >/dev/null || exit 1

# One boot: the CPU model in $1, the session's log in $2.
session() {
    local cpu=$1 log=$2 qp

    rm -f "$log" "$FIFO"
    mkfifo "$FIFO"
    "$QEMU" -M sage040 -cpu "$cpu" -m "$RAM_MB" \
        -kernel ../bootrom/bootrom.elf \
        -drive file="$DISK",format=raw,if=ide \
        -display none -no-reboot \
        -chardev stdio,id=con,signal=off -serial chardev:con \
        < "$FIFO" > "$log" 2>&1 &
    qp=$!
    exec 3> "$FIFO"

    wait_for() {
        for _ in $(seq 1 300); do
            grep -qF "$1" "$log" 2>/dev/null && return 0
            kill -0 "$qp" 2>/dev/null || return 1
            sleep 0.2
        done
        return 1
    }

    sleep "$BOOT_WAIT"
    # Each marker is typed with an empty pair of quotes in it, so that
    # only the command's OUTPUT matches, never its echo.
    printf "echo BOO''TED\r" >&3; wait_for BOOTED
    printf 'fpsptest > /one.out\r' >&3
    printf "echo ONE-DO''NE\r" >&3; wait_for ONE-DONE
    if [ "$cpu" != m68040 ]; then
        printf 'fpsptest > /a.out & fpsptest > /b.out\r' >&3
        printf "wait; echo TWO-DO''NE\r" >&3; wait_for TWO-DONE
        printf 'fpsptest -i\r' >&3
        printf "echo ILL-DO''NE\r" >&3; wait_for ILL-DONE
    fi
    printf "echo ALL-DO''NE\r" >&3; wait_for ALL-DONE
    exec 3>&-
    kill "$qp" 2>/dev/null
    wait "$qp" 2>/dev/null
    rm -f "$FIFO"
}

# Gone before either boot, so no check can read a previous run's.
rm -f "$LOG" "$LOG.native" "$LOG.trap"

prepare() {
    rm -f "$DISK"
    dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
    printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
        | sfdisk -q "$DISK" >/dev/null
    fsimg mkfs SAGE040 >/dev/null
    fsimg put kernel.rom /KERNEL.ROM >/dev/null
    fsimg mkdir /bin >/dev/null
    fsimg put -m 755 ../apps/fpsptest /bin/fpsptest >/dev/null
}

get() { fsimg get "$1" "$2" >/dev/null 2>&1 || : > "$2"; }

ncases=$(grep -c '^    { "' ../apps/fpsptest.c)
ncalls=$((ncases + 2))         # sincos counts once; fmovecr once

echo "=== the CPU computing (QEMU softfloat) ==="
prepare
session m68040 "$LOG.native"
get /one.out "$SCRATCH/fpsp-native.out"
sed 's/^/  | /' "$SCRATCH/fpsp-native.out" | grep -v '  ok  '

grep -q '^fpsptest: all right' "$SCRATCH/fpsp-native.out"
check "every function agrees with the host's libm" $?
grep -q '^fpsptest: 0 instructions completed by the FPSP' \
    "$SCRATCH/fpsp-native.out"
check "  and the FPSP was never asked" $?

echo "=== the FPSP computing (-cpu m68040,fpsp-trap=on) ==="
prepare
session m68040,fpsp-trap=on "$LOG.trap"
get /one.out "$SCRATCH/fpsp-trap.out"
get /a.out "$SCRATCH/fpsp-a.out"
get /b.out "$SCRATCH/fpsp-b.out"
sed 's/^/  | /' "$SCRATCH/fpsp-trap.out" | grep -v '  ok  '

grep -q '^fpsptest: all right' "$SCRATCH/fpsp-trap.out"
check "every function agrees with the host's libm" $?
n=$(sed -n 's/^fpsptest: \([0-9]*\) instructions completed by the FPSP.*/\1/p' \
    "$SCRATCH/fpsp-trap.out")
echo "  the FPSP completed ${n:-no} instructions; the program made $ncalls calls"
[ "${n:-0}" -eq "$ncalls" ]
check "  and it was the FPSP that computed every one" $?

grep -q '^fpsptest: all right' "$SCRATCH/fpsp-a.out" &&
    grep -q '^fpsptest: all right' "$SCRATCH/fpsp-b.out"
check "two copies at once, switching tasks mid-emulation, both right" $?

tr -d '\r' < "$LOG.trap" | grep -q '^fpsptest: SIGILL, as it should be'
check "an F-line instruction that is not floating point is SIGILL" $?

! cat "$LOG.native" "$LOG.trap" | tr -d '\r' | grep -qE 'panic|FPSP:'
check "no panic, and no FPSP complaint" $?

cat "$LOG.native" "$LOG.trap" > "$LOG"
echo
echo "--- $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ "$fail" -eq 0 ]
