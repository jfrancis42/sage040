#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# pagetest.sh - demand paging, copy-on-write, and swap (task 21).
#
# Two machines. The first has the usual memory and no swap:
#
#   a 32 MB mapping and a 16 MB heap cost their page tables and no more
#   until touched; a fork of a 1 MB process shares it, and one write
#   copies one page; mmap refuses what memory could never supply; two
#   processes that were each promised memory and then both touch it are
#   killed -- and the machine is not, and gets every page back.
#
# The second has 12 MB of memory and a 24 MB swap file:
#
#   16 MB filled and read back intact, by one process and by two at
#   once; a fork with pages out; the swap file refusing to be deleted,
#   overwritten or moved while in use; swapoff refused with pages it
#   cannot bring back -- and those pages still intact -- then allowed.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"

. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-page.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/pagetest.log"
# Gone before QEMU starts, so a run that never reaches the guest has no
# log to grade -- rather than silently grading the last run's.
rm -f "$LOG"
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
make -s -C ../apps || exit 1

# boot RAM_MB DISK_MB SWAP_MB: a fresh disk, and the machine running on it.
boot() {
    local ram=$1 disk_mb=$2 swap_mb=$3
    echo "=== a machine with $ram MB, ${swap_mb:-no} swap ==="
    rm -f "$DISK" "$LOG"
    dd if=/dev/zero of="$DISK" bs=1M count="$disk_mb" status=none
    printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
        | sfdisk -q "$DISK" >/dev/null
    fsimg mkfs SAGE040
    fsimg put kernel.rom /KERNEL.ROM
    fsimg mkdir /bin
    fsimg put -m 755 ../apps/pagetest /pagetest
    fsimg put -m 755 ../system/swapon /bin/swapon
    fsimg put -m 755 ../system/swapoff /bin/swapoff
    if [ "$swap_mb" -gt 0 ]; then
        # `alloc`, not a dd of zeroes put in with `put`: debugfs writes
        # an all-zero file as a fully SPARSE one -- the size is right
        # and it has no blocks at all -- and swapon maps every page
        # through the filesystem's bmap, which a hole has no block to
        # answer with. Linux refuses a swap file with holes for the
        # same reason, and so does this kernel.
        fsimg alloc /SWAP "$swap_mb"
    fi
    rm -f "$SCRATCH/in.fifo"
    mkfifo "$SCRATCH/in.fifo"
    "$QEMU" -M sage040 -cpu m68040 -m "$ram" \
        -kernel ../bootrom/bootrom.elf \
        -drive file="$DISK",format=raw,if=ide \
        -display none -no-reboot \
        -chardev stdio,id=con,signal=off -serial chardev:con \
        < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
    qemu_pid=$!
    exec 3> "$SCRATCH/in.fifo"
    sleep "$BOOT_WAIT"
}

finish() {
    exec 3>&-
    kill "$qemu_pid" 2>/dev/null
    wait "$qemu_pid" 2>/dev/null
    rm -f "$SCRATCH/in.fifo"
    tr -d '\r' < "$LOG" > "$1"
    sed -n '/kernel ready/,$p' "$1" | sed 's/^/  | /'
}

wait_for() {
    for _ in $(seq 1 600); do
        if grep -qF "$1" "$LOG" 2>/dev/null; then return 0; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2"
    sleep 0.2
}

# The output between the LAST prompt naming a command and its marker.
between() {
    awk -v s="$1" -v e="DONE-$2" '
        $0 == e { printf "%s", buf; exit }
        index($0, "$ " s) || $0 == s { buf = ""; f = 1; next }
        f { buf = buf $0 "\n" }' "$3"
}

# The "N job(s)" figure from `free`, within a section.
jobs_of() {
    between free "$1" "$2" | sed -n 's/.* \([0-9]*\) job(s)$/\1/p'
}

# The "used" figure from `free`, within a section.
used_of() {
    between free "$1" "$2" | awk '$1 == "used" { print $2; exit }'
}

# A program that died part way prints fewer lines and so FAILS fewer
# checks: grading only what appeared once let a pagetest killed by the
# out-of-memory path pass. So every graded section must also have got to
# its own "N failed" line.
grade() {                      # grade SECTION-COMMAND MARKER FILE
    between "$1" "$2" "$3" | grep -qE '^pagetest: [0-9]+ failed$'
    check "$1 ran to the end" $?
    while IFS= read -r line; do
        case "$line" in
            "  ok   "*)   check "${line#  ok   }" 0 ;;
            "  FAIL "*)   check "${line#  FAIL }" 1 ;;
        esac
    done < <(between "$1" "$2" "$3" | grep -E '^  (ok  |FAIL) ')
}

A="$SCRATCH/page-a.tmp"
B="$SCRATCH/page-b.tmp"

# --- the first machine: plenty of memory, no swap --------------------
boot "$RAM_MB" 16 0
run 'free' free0
run '/pagetest lazy' lazy
run '/pagetest cow' cow
run '/pagetest hog; echo HOG=$?' hog
run "/pagetest oom $(( RAM_MB * 3 / 4 )); echo OOM=\$?" oom
# When the PARENT is the one killed, its child is orphaned mid-way
# through touching its memory, and still running when the shell prompts
# again -- so "every page came back" is only a fair question once the
# job count is back where it started. If the orphan never goes, the
# check below fails, as it should.
tr -d '\r' < "$LOG" > "$A"
j0=$(jobs_of free0 "$A")
for k in $(seq 1 20); do
    run 'free' "freew$k"
    tr -d '\r' < "$LOG" > "$A"
    [ "$(jobs_of "freew$k" "$A")" = "$j0" ] && break
    sleep 0.5
done
run 'free' free1
finish "$A"

# --- the second: 12 MB, and swap --------------------------------------
boot 12 48 24
run 'free' sfree0
run 'swapon /SWAP; echo SWAPON=$?' swapon
run 'free' sfree1
run '/pagetest fill 16' fill
run '/pagetest pair 8' pair
# Again with a slow disk: every page written out or read back sleeps
# 5 ms more, so the other process runs while pages are half way out.
run '/pagetest delay 5' delay5
run '/pagetest pair 8' slowpair
run '/pagetest delay 0' delay0
run '/pagetest forkswap 14' forkswap
run '/pagetest pinread 16' pinread
run '/pagetest stats' stats1
run 'rm /SWAP' rm
run 'cp /pagetest /SWAP' cp
run 'mv /SWAP /S2' mv
run '/pagetest park 14 12 &' park
sleep 10
run 'swapoff /SWAP; echo OFF1=$?' off1
# Its result, and then its exit: it prints a moment before its pages
# are given back, so what is waited for is the swap file being empty.
wait_for "park: pages that came back wrong"
run '/pagetest drained' drained
run 'swapoff /SWAP; echo OFF2=$?' off2
run 'free' sfree2
run '/pagetest stats' stats
finish "$B"

echo "=== checks: demand paging and copy-on-write ==="
grade '/pagetest lazy' lazy "$A"
grade '/pagetest cow' cow "$A"
between '/pagetest hog' hog "$A" | grep -q "mmap refused after MB"
check "mmap refuses what memory could never supply" $?
between '/pagetest hog' hog "$A" | grep -qx "HOG=3"
check "  and says so, rather than the program being killed" $?
between '/pagetest oom' oom "$A" | grep -q "^out of memory at 0x[0-9a-f]*: killed"
check "memory promised twice over and then touched: out of memory, killed" $?
between '/pagetest oom' oom "$A" | grep -qE "^OOM=(0|137)$"
check "  by SIGKILL, or the child was and the parent survived" $?
u0=$(used_of free0 "$A"); u1=$(used_of free1 "$A")
echo "  used pages: before=${u0:-?} after=${u1:-?}"
[ -n "$u0" ] && [ "$u0" = "$u1" ]
check "  and every page came back: used before and after the same" $?
! grep -q "panic\|exception" "$A"
check "no panic, no kernel exception" $?

echo "=== checks: swap ==="
between 'swapon /SWAP' swapon "$B" | grep -qx "SWAPON=0"
check "swapon" $?
between free sfree1 "$B" | grep -qE '^swap +6144 +24576 +0 pages in use$'
check "  and free shows 24 MB of it, none used" $?
grade '/pagetest fill' fill "$B"
grade '/pagetest pair' pair "$B"
grade '/pagetest pair' slowpair "$B"
grade '/pagetest forkswap' forkswap "$B"
grade '/pagetest pinread' pinread "$B"
between '/pagetest stats' stats1 "$B" | grep -qx "pagetest: swap-used 0"
check "every slot given back once its processes had gone" $?
between 'rm /SWAP' rm "$B" | grep -q "text file busy"
check "the swap file cannot be deleted while in use" $?
between 'cp /pagetest /SWAP' cp "$B" | grep -q "text file busy"
check "  nor overwritten" $?
between 'mv /SWAP' mv "$B" | grep -q "text file busy"
check "  nor moved" $?
between 'swapoff /SWAP; echo OFF1=$?' off1 "$B" | grep -q "not enough memory" &&
    between 'swapoff /SWAP; echo OFF1=$?' off1 "$B" | grep -qx "OFF1=1"
check "swapoff with more out than memory can hold: refused" $?
grep -q "park: pages that came back wrong: 0" "$B"
check "  and the parked process's pages are all still intact" $?
between '/pagetest drained' drained "$B" | grep -qx "pagetest: swap drained"
check "  and when it exits, its slots are given back" $?
between 'swapoff /SWAP; echo OFF2=$?' off2 "$B" | grep -qx "OFF2=0"
check "swapoff once they have gone" $?
! between free sfree2 "$B" | grep -q '^swap'
check "  and free shows no swap" $?
out=$(between '/pagetest stats' stats "$B" | sed -n 's/^pagetest: pageouts //p')
[ "${out:-0}" -gt 0 ]
check "pages were written out ($out)" $?
! grep -q "panic\|exception" "$B"
check "no panic, no kernel exception" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
