#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# usertest.sh - users, /etc/passwd, home directories (tasks 33, 34, 35).
#
# WHAT IS AND IS NOT BEING CLAIMED. A task now carries a real,
# effective and saved user and group id; they are inherited across
# fork and exec, setuid() moves them under POSIX's rules, and
# /etc/passwd turns them into names. What none of it does is stop
# anybody reading anybody's files: a FAT directory entry has nowhere
# to record an owner, so there is no permission to enforce. This suite
# tests identity, and the last check tests that the absence of
# enforcement is honest rather than accidental -- a test that quietly
# assumed protection would be worse than no test.
#
# The C library's getpwuid() is exercised through sbase's `whoami`,
# which is a different code path from the shell's own passwd reader:
# the shell parses /etc/passwd itself (it may not call libc), so the
# two are independent implementations of the same lookup and are
# checked to agree.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-user.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/usertest.log"
WORK="$SCRATCH/user.tmp"
rm -f "$LOG"; rm -rf "$WORK"; mkdir -p "$WORK"
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
make -s -C ../system id df sh || exit 1
make -s -C ../ldso || exit 1
[ -x ../ports/sbase/bin/whoami ] || ../ports/sbase/build.sh > /dev/null || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /etc; fsimg mkdir /home; fsimg mkdir /home/jfrancis; fsimg mkdir /root; fsimg mkdir /ST
fsimg put -m 755 ../system/sh /bin/sh
fsimg put -m 755 ../system/id /bin/id
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../system/passwd /etc/passwd
fsimg put -m 755 ../system/group /etc/group
for p in whoami ls echo cat pwd; do
    [ -x "../ports/sbase/bin/$p" ] && \
        fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p
done
echo "belongs to root" > "$WORK/rootfile.txt"
echo "belongs to jfrancis" > "$WORK/jefffile.txt"
fsimg put "$WORK/rootfile.txt" /root/rootfile.txt
fsimg put "$WORK/jefffile.txt" /home/jfrancis/jefffile.txt

rm -f "$SCRATCH/user.fifo"; mkfifo "$SCRATCH/user.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/user.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/user.fifo"

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
run 'id > /ST/id.out'                          id
run 'whoami > /ST/whoami.out'                  whoami
run 'id -u > /ST/idu.out'                      idu
run 'id -u -n > /ST/idun.out'                  idun
run 'echo $HOME > /ST/home.out'                home
run 'echo ~ > /ST/tilde.out'                   tilde
run 'echo ~jfrancis > /ST/tj.out'              tj
run 'echo ~root > /ST/tr.out'                  tr
run 'echo ~nosuchuser > /ST/tn.out'            tn
run 'echo mid=a~b > /ST/mid.out'               mid
run 'echo "quoted ~" > /ST/quoted.out'         quoted
run 'cat ~jfrancis/jefffile.txt > /ST/cat.out' cat
run 'cd ~jfrancis'                             cd
run 'pwd > /ST/pwd.out'                        pwd
run 'cd /'                                     cdback
# Anybody may read anybody's file, and the suite says so on purpose.
run 'cat /root/rootfile.txt > /ST/nofence.out'  nofence
run 'echo ALL-DONE'                            end
wait_for "ALL-DONE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/user.fifo"
tr -d '\r' < "$LOG" > "$WORK/session.txt"

get() { fsimg get /ST/$1 $WORK/$1 2>/dev/null; }
for f in id.out whoami.out idu.out idun.out home.out tilde.out tj.out \
         tr.out tn.out mid.out quoted.out cat.out pwd.out nofence.out; do
    get "$f"
done
say() { tr -d '\r\n' < "$WORK/$1" 2>/dev/null; }

echo "=== checks ==="

! grep -qE "panic|exception at|DOUBLE MMU" "$WORK/session.txt"
check "no panic, no kernel exception" $?

# --- identity ---------------------------------------------------------
grep -q "uid=0(root)" "$WORK/id.out" 2>/dev/null
check "id reports uid 0 and resolves it to root" $?

grep -q "gid=0(root)" "$WORK/id.out" 2>/dev/null
check "  and the group as well" $?

test "$(say idu.out)" = "0"
check "id -u prints the number alone" $?

test "$(say idun.out)" = "root"
check "id -u -n prints the name" $?

# The C library's own lookup, which is a different implementation from
# the shell's: whoami calls getpwuid(), the shell parses the file.
test "$(say whoami.out)" = "root"
check "whoami agrees, through the C library's getpwuid()" $?

# --- /etc/passwd and homes -------------------------------------------
test "$(say home.out)" = "/root"
check "HOME comes from /etc/passwd, not from a compiled-in default" $?

test "$(say tilde.out)" = "/root"
check "a bare ~ is HOME" $?

test "$(say tj.out)" = "/home/jfrancis"
check "~jfrancis is that user's home from /etc/passwd" $?

test "$(say tr.out)" = "/root"
check "~root likewise" $?

test "$(say tn.out)" = "~nosuchuser"
check "~ of a user who does not exist is left exactly as typed" $?

test "$(say mid.out)" = "mid=a~b"
check "a tilde in the middle of a word is not expanded" $?

grep -q '~' "$WORK/quoted.out" 2>/dev/null
check "a quoted tilde stays a tilde" $?

test "$(say cat.out)" = "belongs to jfrancis"
check "~user/path reaches the file" $?

test "$(say pwd.out)" = "/home/jfrancis"
check "cd ~jfrancis goes there" $?

# --- and what is NOT true --------------------------------------------
#
# THE POINT OF THIS CHECK is that it passes. There are user ids, and
# they protect nothing: FAT cannot record an owner, so root's file is
# readable by anybody and would be even if this were not running as
# root. A suite that left this out could be read as evidence of a
# protection that does not exist.
test "$(say nofence.out)" = "belongs to root"
check "files are NOT protected by owner -- FAT has none (task 36)" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
