#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# sotest.sh - shared libraries: /lib/ld.so, /lib/libc.so, and the
# kernel's sharing of their pages.
#
#   sotest (libc/test/sotest.c), linked against libsot.so and libc.so:
#   a library's function, variable, constructor, and its own call into
#   libc; the physical page behind libc's text the same in two separately
#   exec'd programs and in a forked child; the library's data private;
#   mprotect(PROT_WRITE) giving one process its own copy and nobody else.
#
#   Then from the shell: replacing a library on the disk -- by cp, which
#   truncates, and by writing over it in place, which does not -- is
#   seen by the next program (the kernel's cache forgets the file);
#   LD_LIBRARY_PATH; LD_TRACE_LOADED_OBJECTS; a missing library, a
#   missing symbol and a missing ld.so, each refused before main.
#
#   And libctest -- the whole C library test -- linked dynamically.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"

. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-so.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/sotest.log"
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

T=../libc/test

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../ldso || exit 1
make -s -C $T all libctest.dyn || exit 1

# The in-place rewrite puts libsot.so's bytes over libsot2.so's, which
# only means anything if nothing of the old file is left past the end.
test "$(stat -c %s $T/libsot.so)" -eq "$(stat -c %s $T/libsot2.so)" || {
    echo "sotest.sh: libsot.so and libsot2.so differ in size" >&2
    exit 1
}

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /opt
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put $T/libsot.so /lib/libsot.so
fsimg put $T/libsot.so /opt/libsot.so
fsimg put $T/libsot2.so /libsot2.so
fsimg put $T/libsot3.so /libsot3.so
fsimg put $T/libsot2.so /victim.so
# 8.3 names in upper case, so that no long-name entries are made, and
# moves from ANOTHER directory, which make a new entry in the first free
# slot: the slot a deleted or replaced file left, and so its inode
# number: on ext2 each of these is a different file and so a different
# inode, and the last step makes a NEW file take a number just freed.
fsimg mkdir /D
fsimg put $T/libsot2.so /D/A.SO
head -c 8192 /dev/zero | tr '\0' B > "$SCRATCH/bbbb.tmp"
head -c 8192 /dev/zero | tr '\0' C > "$SCRATCH/cccc.tmp"
head -c 8192 /dev/zero | tr '\0' E > "$SCRATCH/eeee.tmp"
fsimg put "$SCRATCH/bbbb.tmp" /B.SO
fsimg put "$SCRATCH/cccc.tmp" /C.SO
fsimg put "$SCRATCH/eeee.tmp" /E.SO
rm -f "$SCRATCH/bbbb.tmp" "$SCRATCH/cccc.tmp" "$SCRATCH/eeee.tmp"
fsimg put $T/sotest /sotest
# The same program, set-user-id to somebody who is NOT root. Run by
# root, that makes uid 0 and euid 1000 -- which is what ld.so uses to
# decide that it must not take a library path from the environment.
# Owned by root it would prove nothing: uid and euid would both be 0.
fsimg put -m 4755 $T/sotest /sosetuid
fsimg chown /sosetuid 1000:1000
fsimg put $T/libctest.dyn /libctest.dyn

rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/in.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/in.fifo"
sleep "$BOOT_WAIT"

wait_for() {
    for _ in $(seq 1 300); do
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

run '/sotest' full
run '/sotest value' v1
run 'cp /libsot2.so /lib/libsot.so' swap
run '/sotest value' v2
run 'export LD_LIBRARY_PATH=/nowhere:/opt' lpset
run '/sotest value' vpath
run '/sosetuid value' vsuid
run 'unset LD_LIBRARY_PATH' lpunset
run '/sotest overwrite /opt/libsot.so /lib/libsot.so' ovw
run '/sotest value' vover
run '/sotest mapfirst /victim.so' map1
run '/sotest truncate /victim.so' trunc
run '/sotest mapfirst /victim.so' map2
run '/sotest mapfirst /D/A.SO' mapa
run 'rm /D/A.SO' rma
run 'mv /B.SO /D/A.SO' mvba
run '/sotest mapfirst /D/A.SO' mapb
run 'mv /C.SO /D/A.SO' mvca
run '/sotest mapfirst /D/A.SO' mapc
run 'rm /D/A.SO' rmc
run 'cp /E.SO /D/A.SO' cpe
run '/sotest mapfirst /D/A.SO' mapd
run 'export LD_TRACE_LOADED_OBJECTS=1' trset
run '/sotest' trace
run 'unset LD_TRACE_LOADED_OBJECTS' trunset
run 'cp /libsot3.so /lib/libsot.so' swap3
run '/sotest value; echo SYM-RC=$?' nosym
run 'rm /lib/libsot.so' rmlib
run '/sotest value; echo LIB-RC=$?' nolib
run 'mv /lib/ld.so /lib/ld.bak' mvld
run '/sotest value' nold
run 'mv /lib/ld.bak /lib/ld.so' mvback
run '/libctest.dyn' libc

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo"
tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"

echo "=== guest session ==="
sed -n '/kernel ready/,$p' "$SCRATCH/clean.tmp" | sed 's/^/  | /'

# The output between a command and its DONE marker.
# The same command is run more than once here, so this takes the LAST
# line naming it before the marker, not the first in the log.
between() {
    awk -v s="$1" -v e="DONE-$2" '
        $0 == e { printf "%s", buf; exit }
        index($0, "$ " s) { buf = ""; f = 1; next }
        f { buf = buf $0 "\n" }' "$SCRATCH/clean.tmp"
}

echo "=== checks: sotest ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(between '/sotest' full | grep -E '^  (ok  |FAIL) ')
between '/sotest' full | grep -qx "sotest: done"
check "sotest ran to the end" $?

echo "=== checks: from the shell ==="
between '/sotest value' v1 | grep -qx "sotest: value 1"
check "libsot.so as installed" $?
between '/sotest value' v2 | grep -qx "sotest: value 2"
check "replaced on the disk, and the next program gets the new one" $?
between '/sotest value' vpath | grep -qx "sotest: value 1"
check "LD_LIBRARY_PATH is searched first, past a directory that is not there" $?

# AND A SET-USER-ID PROGRAM MUST NOT LISTEN TO IT. The line above is
# this one's positive control: the same variable, the same /opt, and an
# ordinary program does take the library from there. A program whose
# euid is not its uid must take /lib's instead -- otherwise whoever
# runs it chooses the code that runs with the privilege, which is the
# oldest hole in dynamic linking.
between '/sosetuid value' vsuid | grep -qx "sotest: value 2"
check "  but a set-user-id program ignores it, and gets /lib's library" $?
between '/sotest overwrite' ovw | grep -q "sotest: overwrote" &&
    between '/sotest value' vover | grep -qx "sotest: value 1"
check "rewritten in place, not truncated, and the next program sees it" $?
between '/sotest mapfirst' map1 | grep -qx "sotest: first word 7f454c46" &&
    between '/sotest mapfirst' map2 | grep -qx "sotest: first word 00000000"
check "truncated and not written: a mapping shows zeroes, not the old bytes" $?
ino_a=$(between '/sotest mapfirst' mapa | sed -n 's/^sotest: inode //p')
ino_b=$(between '/sotest mapfirst' mapb | sed -n 's/^sotest: inode //p')
ino_c=$(between '/sotest mapfirst' mapc | sed -n 's/^sotest: inode //p')
ino_d=$(between '/sotest mapfirst' mapd | sed -n 's/^sotest: inode //p')
# On ext2 an inode number belongs to the FILE, for its whole life: a
# different file under the same name is a different inode, whatever the
# directory did. (On FAT the number came from the directory slot, so
# these three were equal and the cache could not tell them apart.)
[ -n "$ino_a" ] && [ "$ino_a" != "$ino_b" ] && [ "$ino_b" != "$ino_c" ]
check "  (each file under the name has its own inode: $ino_a $ino_b $ino_c)" $?

# ...but a number given up IS handed out again, and that is the case a
# cache keyed on the inode number alone gets wrong. The file below is a
# new file that took a number one of the others had freed -- the LOWEST
# free one, which is not necessarily the one freed most recently, so the
# check is that it is one of them rather than which.
[ -n "$ino_d" ] &&
    { [ "$ino_d" = "$ino_a" ] || [ "$ino_d" = "$ino_b" ] ||
      [ "$ino_d" = "$ino_c" ]; }
check "  (a freed inode number is handed out again: $ino_d was one of $ino_a $ino_b $ino_c)" $?
between '/sotest mapfirst' mapd | grep -qx "sotest: first word 45454545"
check "a NEW file reusing a freed inode number shows its own bytes, not the last file's" $?
between '/sotest mapfirst' mapa | grep -qx "sotest: first word 7f454c46" &&
    between '/sotest mapfirst' mapb | grep -qx "sotest: first word 42424242"
check "a file deleted, another moved into its slot: the new one's bytes" $?
between '/sotest mapfirst' mapc | grep -qx "sotest: first word 43434343"
check "a move over an existing file: the new one's bytes" $?
between '/sotest' trace | grep -qE '^	libsot.so => /lib/libsot.so \(0x[0-9a-f]{8}\)$' &&
    between '/sotest' trace | grep -qE '^	libc.so => /lib/libc.so \(0x[0-9a-f]{8}\)$'
check "LD_TRACE_LOADED_OBJECTS lists both libraries" $?
! between '/sotest' trace | grep -q "sotest: two shared"
check "  and does not run the program" $?
between '/sotest value' nosym | grep -qx "ld.so: undefined symbol: sot_bump" &&
    between '/sotest value' nosym | grep -qx "SYM-RC=127"
check "a symbol no library defines is refused at start, status 127" $?
! between '/sotest value' nosym | grep -q "sotest: value"
check "  before main" $?
between '/sotest value' nolib | grep -qx "ld.so: cannot find library: libsot.so" &&
    between '/sotest value' nolib | grep -qx "LIB-RC=127"
check "a missing library is refused, status 127" $?
between '/sotest value' nold | grep -q "cannot access a needed shared library"
check "a missing ld.so: the exec fails with ELIBACC" $?

echo "=== checks: libctest, linked against libc.so ==="
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "dyn: ${line#  ok   }" 0 ;;
        "  FAIL "*)   check "dyn: ${line#  FAIL }" 1 ;;
    esac
done < <(between '/libctest.dyn' libc | grep -E '^  (ok  |FAIL) ')
test "$(between '/libctest.dyn' libc | grep -cE '^  ok   ')" -ge 60
check "dyn: libctest ran all of its checks" $?
between '/libctest.dyn' libc | grep -qx "libctest: atexit handler ran"
check "dyn: atexit handlers run, from libc.so's exit" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
