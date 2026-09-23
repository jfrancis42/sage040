#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# dftest.sh - df and du: how much of the disk is gone, and where.
#
# WHAT MAKES THIS MORE THAN "IT PRINTED SOMETHING". A df that prints a
# plausible table while reporting the wrong numbers is worse than one
# that fails, because nobody checks it twice. So the numbers the
# machine reports are compared against what the HOST's own tools say
# about the same image: mdir gives the free space, and the files were
# put there by the host, which therefore knows how big they are.
#
# The known sizes are chosen to be awkward on purpose. A file of
# 300,000 bytes is not a whole number of 1 KB blocks or of clusters,
# so a du that rounds the wrong way, or reports bytes as though they
# were blocks, cannot come out right by accident.
#
# THE NEGATIVE CONTROL is the last check: /bin/df is moved aside and
# `df` run again. The shell has a df BUILT IN, which must then answer
# -- and must NOT have been answering all along, which is what the
# different heading proves. Without this check, every df test above
# would pass just as well if the external program had never run.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-df.img"
PART_LBA=2048
MIMG="$DISK@@$((PART_LBA * 512))"
LOG="$SCRATCH/dftest.log"
WORK="$SCRATCH/df.tmp"
rm -f "$LOG"
rm -rf "$WORK"
mkdir -p "$WORK"
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
make -s -C ../system df sh || exit 1
make -s -C ../ldso || exit 1
if [ ! -x ../ports/sbase/bin/du ]; then
    ../ports/sbase/build.sh > /dev/null || exit 1
fi

# Sizes that are not round numbers in any unit involved.
BIG1=300000
BIG2=70000

DF_MB=64
echo "=== preparing $DISK ($DF_MB MB) ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=$DF_MB status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (DF_MB * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mmd -i "$MIMG" ::/BIN ::/lib ::/ST ::/ST/sub
mcopy -o -i "$MIMG" ../system/sh ::/BIN/sh
mcopy -o -i "$MIMG" ../system/df ::/BIN/df
mcopy -o -i "$MIMG" ../ldso/ld.so ::/lib/ld.so
mcopy -o -i "$MIMG" "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" ::/lib/libc.so
for p in du ls cat echo mv; do
    [ -x "../ports/sbase/bin/$p" ] && \
        mcopy -o -i "$MIMG" "../ports/sbase/bin/$p" "::/BIN/$p"
done
head -c $BIG1 /dev/urandom > "$WORK/big1.bin"
head -c $BIG2 /dev/urandom > "$WORK/big2.bin"
mcopy -o -i "$MIMG" "$WORK/big1.bin" ::/ST/big1.bin
mcopy -o -i "$MIMG" "$WORK/big2.bin" ::/ST/sub/big2.bin

# What the HOST says about the same volume, before the machine boots.
host_free=$(mdir -i "$MIMG" ::/ 2>/dev/null | awk '/bytes free/ {gsub(/[^0-9]/,"",$0); print}')
echo "    the host says $host_free bytes free"

rm -f "$SCRATCH/df.fifo"
mkfifo "$SCRATCH/df.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/df.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/df.fifo"

wait_for() {
    for _ in $(seq 1 900); do
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
run 'df > /ST/df.out'            df
run 'df -h > /ST/dfh.out'        dfh
run 'df -i > /ST/dfi.out'        dfi
run 'du -k /ST > /ST/duk.out'    duk
run 'du -h /ST > /ST/duh.out'    duh
run 'du -sk /ST > /ST/dusk.out'  dusk
run 'du -ak /ST > /ST/duak.out'  duak
# The negative control: with no /bin/df, the SHELL must answer.
run 'mv /BIN/df /BIN/dfmoved'    move
run 'df > /ST/dfbuilt.out'       builtin
run 'echo ALL-DONE'              end
wait_for "ALL-DONE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/df.fifo"
tr -d '\r' < "$LOG" > "$WORK/session.txt"

for f in df.out dfh.out dfi.out duk.out duh.out dusk.out duak.out dfbuilt.out; do
    mcopy -n -o -i "$MIMG" "::/ST/$f" "$WORK/$f" 2>/dev/null
done

echo "=== what the machine printed ==="
for f in df.out dfh.out dfi.out duk.out duh.out dusk.out duak.out dfbuilt.out; do
    echo "--- $f"; sed 's/^/  | /' "$WORK/$f" 2>/dev/null
done

echo "=== checks ==="

! grep -qE "panic|exception at|DOUBLE MMU" "$WORK/session.txt"
check "no panic, no kernel exception" $?

# --- df ---------------------------------------------------------------
grep -q "mounted on" "$WORK/df.out" 2>/dev/null
check "df ran the PROGRAM, not the shell's built-in" $?

grep -qi "SAGE040" "$WORK/df.out" 2>/dev/null
check "  and named the volume by its label" $?

# The free space the machine reports, against the host's own figure for
# the same image. They are taken at different moments -- the machine
# wrote a few small files -- so they must be close, not identical.
mach_avail=$(awk 'NR==2 {print $4}' "$WORK/df.out" 2>/dev/null)
host_avail_k=$((host_free / 1024))
if [ -n "${mach_avail:-}" ] && [ "$host_avail_k" -gt 0 ]; then
    diff=$(( mach_avail > host_avail_k ? mach_avail - host_avail_k
                                       : host_avail_k - mach_avail ))
    # Within 1% of the host's answer.
    test "$diff" -lt $(( host_avail_k / 100 + 64 ))
    check "  free space agrees with the HOST's mdir ($mach_avail K vs $host_avail_k K)" $?
else
    check "  free space agrees with the host's mdir" 1
fi

# used + avail must be the total, or the arithmetic is not arithmetic.
awk 'NR==2 { exit !($2 == $3 + $4 || $2 - ($3 + $4) < 2) }' "$WORK/df.out"
check "  total = used + available" $?

grep -qE "[0-9]+M" "$WORK/dfh.out" 2>/dev/null
check "df -h reports megabytes, not raw blocks" $?

grep -qi "no inode table" "$WORK/dfi.out" 2>/dev/null
check "df -i says FAT has no inodes rather than printing zeroes" $?

# --- du ---------------------------------------------------------------
# 300000 bytes is 293 KB; 70000 is 69 KB. du -k must say so.
awk '$2 ~ /big1/ { exit !($1 >= 293 && $1 <= 300) }' "$WORK/duak.out"
check "du -ak reports the 300000-byte file as ~293 K" $?

awk '$2 ~ /sub$/ { exit !($1 >= 69 && $1 <= 76) }' "$WORK/duk.out"
check "du -k reports the subdirectory as ~69 K" $?

# The total must be at least the sum of the parts.
awk '$2 == "/ST" { exit !($1 >= 362 && $1 <= 380) }' "$WORK/duk.out"
check "  and the total is the two of them together (~362 K)" $?

grep -qE "K|M" "$WORK/duh.out" 2>/dev/null
check "du -h reports a suffix rather than a block count" $?

test "$(wc -l < "$WORK/dusk.out" 2>/dev/null)" -eq 1
check "du -sk prints one line and no more" $?

test "$(wc -l < "$WORK/duak.out" 2>/dev/null)" -gt \
     "$(wc -l < "$WORK/duk.out" 2>/dev/null)"
check "du -a lists files as well as directories" $?

# --- the negative control --------------------------------------------
#
# With /bin/df moved aside, `df` must still answer -- from the shell --
# and its output must be the BUILT-IN's, which says "volume" and not
# "mounted on". If this printed the program's heading, the program was
# never being run and every check above was testing the built-in.
grep -q "volume" "$WORK/dfbuilt.out" 2>/dev/null &&
    ! grep -q "mounted on" "$WORK/dfbuilt.out" 2>/dev/null
check "with /bin/df gone, the shell's built-in answers instead" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
