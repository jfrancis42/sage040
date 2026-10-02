#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# ptesttest.sh - the MMU asked directly (PTESTR, MMUSR) about addresses
# whose answers are known another way, through system/ptest and
# memctl(MEMCTL_PTEST).
#
# Each check names the independent fact it rests on: a program's text
# starts at 0x10000000 and is read-only (lib/user.ld, the dynamic link
# address); the supervisor's map is the identity, so a kernel address
# must come back as its own physical page; address 0 is below every
# program; and the background job's pid is the one the shell printed.
# On every one, the MMU and the kernel's own walk must also agree.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-ptest.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/ptesttest.log"; FIFO="$SCRATCH/ptest.fifo"
rm -f "$LOG"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "ptesttest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh ptest || exit 1
make -s -C ../auth || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
# The top of a program's stack, from the header that defines it.
STACK=$(printf '%x' $(( $(sed -n 's/^#define USER_VA_SIZE *\(0x[0-9a-fA-F]*\).*/\1/p' vm.h) +
                        0x10000000 - 16 )))

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=24 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /etc /lib /root /home /home/jfrancis /tmp; do fsimg mkdir $d; done
put_shells
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../auth/login /bin/login
fsimg put -m 4755 ../auth/su /bin/su
fsimg put ../system/passwd /etc/passwd
fsimg put ../system/group /etc/group
fsimg put -m 600 ../system/shadow /etc/shadow
fsimg put ../system/profile /etc/profile
for p in sleep true; do fsimg put -m 755 ../ports/sbase/bin/$p /bin/$p; done
fsimg put -m 755 ../system/ptest /bin/ptest

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
        sleep 0.1
    done
    return 1
}
run() { printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3; wait_for "$1-END [0-9]+" "${3:-600}"; }

wait_for 'login: ?' 900
printf 'root\r' >&3; sleep 2; printf 'root\r' >&3
wait_for '.*[#$] ?' 300
sleep 1
run START 'echo started'
run SLEEP '/bin/sleep 300 &'
bg=$(tr -d '\r' < "$LOG" | sed -n 's|^\[\([0-9]*\)\] */bin/sleep 300 *&$|\1|p' | tail -1)
run SELF "/bin/ptest 10000000 $STACK 0"
run OTHER "/bin/ptest -p ${bg:-0} 10000000"
run SUPER '/bin/ptest -s 100000 4000'
run NOPID '/bin/ptest -p 99999 10000000'
run NOTROOT "/bin/su jfrancis -c '/bin/ptest -s 100000'"
run NOTROOT2 "/bin/su jfrancis -c '/bin/ptest -p ${bg:-0} 10000000'"
run OWN "/bin/su jfrancis -c '/bin/ptest 10000000'"
run BAD '/bin/ptest zzz'
printf 'exit\r' >&3
sleep 2
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CL="$SCRATCH/ptest-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' > "$CL"
echo "=== guest session ==="
sed -n '/started/,$p' "$CL" | sed 's/^/  | /' | head -80
block() { sed -n "/echo \"$1\"-END/,/^$1-END/p" "$CL"; }
status() { sed -n "s/^$1-END //p" "$CL" | tail -1; }
# The three lines ptest prints for ADDR: the address, mmu, walk, verdict.
about() { block "$1" | grep -A3 -ix "0*$2"; }

echo "=== checks ==="
about SELF 10000000 | grep -q 'mmu : resident, page [0-9a-f]* ro user' &&
    about SELF 10000000 | grep -q 'walk: resident, page' &&
    about SELF 10000000 | grep -qx '  agree'
check "its own text: resident, read-only, user -- MMU and walk agree" $?
p1=$(about SELF 10000000 | sed -n 's/.*mmu : resident, page \([0-9a-f]*\).*/\1/p')
p2=$(about SELF 10000000 | sed -n 's/.*walk: resident, page \([0-9a-f]*\).*/\1/p')
[ -n "$p1" ] && [ "$p1" = "$p2" ]; check "  the same physical page from both (${p1:-?})" $?
about SELF "$STACK" | grep -q 'mmu : resident, page [0-9a-f]* rw user' &&
    about SELF "$STACK" | grep -qx '  agree'
check "its own stack top (0x$STACK): resident and writable, agreed" $?
about SELF 0 | grep -q 'mmu : not resident' && about SELF 0 | grep -qx '  agree'
check "address 0: not resident, and the walk says the same" $?
[ "$(status SELF)" = 0 ]; check "  exit 0 when everything agreed" $?
[ -n "$bg" ]; check "a background sleep, pid ${bg:-?}" $?
about OTHER 10000000 | grep -q 'mmu : resident, page [0-9a-f]* ro user' &&
    about OTHER 10000000 | grep -qx '  agree'
check "-p: the sleep's text, in ITS address space, resident and agreed" $?
p3=$(about OTHER 10000000 | sed -n 's/.*mmu : resident, page \([0-9a-f]*\).*/\1/p')
[ -n "$p3" ] && [ "$p3" != "$p1" ]
check "  and a different physical page from ptest's own text ($p3, not $p1)" $?
about SUPER 100000 | grep -qE 'mmu : (resident, page 0*100000 rw super|transparent)' &&
    about SUPER 100000 | grep -qx '  agree'
check "-s: a kernel address is its own physical page, supervisor-only" $?
[ "$(status NOPID)" = 2 ] && block NOPID | grep -q 'no such process'
check "-p of a process that does not exist: refused, exit 2" $?
[ "$(status NOTROOT)" = 2 ] && block NOTROOT | grep -q "root's"
check "-s as an ordinary user: refused (EPERM)" $?
[ "$(status NOTROOT2)" = 2 ] && block NOTROOT2 | grep -q "root's"
check "-p of root's process as an ordinary user: refused" $?
[ "$(status OWN)" = 0 ] && about OWN 10000000 | grep -qx '  agree'
check "  but its own address space is anybody's to ask about" $?
[ "$(status BAD)" = 2 ]; check "a non-hex address is a usage error" $?
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
