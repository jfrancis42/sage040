#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# procfstest.sh - /proc.
#
# Two halves. apps/procfstest runs on the machine and checks /proc
# against what a program knows for itself: its pid, argv, environ, the
# inode of its own file, sysinfo(), where main() and a local variable
# are -- each a different source of the same fact, so a /proc that was
# merely consistent with itself would not pass.
#
# Then ordinary programs are pointed at it, the way ported software
# will be: sbase's cat, ls and readlink, which know nothing about this
# kernel, read /proc through the C library like anything else, and what
# they print is checked here on the host -- against the host's own idea
# of the machine where there is one (how much memory it was given, what
# filesystem the disk holds, the exact bytes a command line must be).
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-procfs.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/procfstest.log"
WORK="$SCRATCH/procfs.tmp"
FIFO="$SCRATCH/procfs.fifo"
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
make -s -C ../system sh || exit 1
make -s -C ../apps procfstest || exit 1
make -s -C ../ldso || exit 1
if [ ! -x ../ports/sbase/bin/readlink ]; then
    ../ports/sbase/build.sh > /dev/null || exit 1
fi

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /tmp; fsimg mkdir /ST
put_shells
fsimg put -m 755 ../apps/procfstest /bin/procfstest
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
for p in cat ls readlink; do
    fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p
done

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
run '/bin/procfstest "two words" three > /ST/pft.out'   pft
# By path: the shell has its own cat and ls, and it is sbase's that
# stand in for ported software here. The shell's ls gets a turn after.
run '/bin/readlink /proc/self/exe > /ST/exe.out'         exe
run '/bin/readlink /proc/self/fd/1 > /ST/fd1.out'        fd1
run '/bin/ls /proc/self > /ST/self.out'                  self
run '/bin/ls /proc > /ST/root.out'                       root
run '/bin/cat /proc/self/cmdline > /ST/cmdline.out'      cmdline
run '/bin/cat /proc/meminfo > /ST/meminfo.out'           meminfo
run '/bin/cat /proc/mounts > /ST/mounts.out'             mounts
run '/bin/cat /proc/self/maps > /ST/maps.out'            maps
run '/bin/cat /proc/self/stat > /ST/stat.out'            stat
run '/bin/ls -l /proc/self/ > /ST/lsl.out'               lsl
run 'ls /proc > /ST/bls.out'                             bls
run 'ls -l /proc/1 > /ST/blsl.out'                       blsl
run 'echo ALL-DONE'                                      end
wait_for "ALL-DONE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"

get() { fsimg get "$1" "$2" >/dev/null 2>&1 || : > "$2"; }
for f in pft exe fd1 self root cmdline meminfo mounts maps stat lsl bls blsl; do
    get "/ST/$f.out" "$WORK/$f.out"
done

echo "=== on the machine: apps/procfstest ==="
sed 's/^/  | /' "$WORK/pft.out"
while IFS= read -r line; do
    case "$line" in
        "  ok   "*)   check "${line#  ok   }" 0 ;;
        "  FAIL "*)   check "${line#  FAIL }" 1 ;;
    esac
done < <(grep -E '^  (ok|FAIL) ' "$WORK/pft.out")
grep -qE '^procfstest: [0-9]+ checks, 0 failed$' "$WORK/pft.out"
check "  and it ran to the end" $?

echo "=== ordinary programs reading it ==="
[ "$(cat "$WORK/exe.out")" = "/bin/readlink" ]
check "readlink /proc/self/exe names readlink itself" $?

# The shell's output, redirected: a file on the volume, by its path.
[ "$(cat "$WORK/fd1.out")" = "/ST/fd1.out" ]
check "readlink /proc/self/fd/1 under a redirection is that file" $?

want_self="cmdline comm cwd environ exe fd maps mem root stat statm status task"
[ "$(tr '\n' ' ' < "$WORK/self.out" | sed 's/ $//')" = "$want_self" ]
check "ls /proc/self lists what a process has" $?

for n in cpuinfo loadavg meminfo mounts self stat uptime; do
    grep -qx "$n" "$WORK/root.out" || { echo "    missing: $n"; false; }
done
check "ls /proc lists the machine's files" $?
grep -qxE '[0-9]+' "$WORK/root.out"
check "  and numbered directories, one per process" $?

# The exact bytes: two arguments, each followed by a NUL.
printf '/bin/cat\0/proc/self/cmdline\0' | cmp -s - "$WORK/cmdline.out"
check "cat /proc/self/cmdline is cat's own argv, NUL after each" $?

# The HOST knows how much memory it gave the machine. MemTotal is what
# the page allocator has after the kernel and its tables, so a little
# less -- but not a lot less, and never more.
total=$(awk '/^MemTotal:/ {print $2}' "$WORK/meminfo.out")
given=$((RAM_MB * 1024))
[ -n "$total" ] && [ "$total" -le "$given" ] && \
    [ "$total" -ge $((given * 9 / 10)) ]
check "MemTotal is within 10% below the ${RAM_MB} MB the host gave (${total:-none} kB)" $?
free_=$(awk '/^MemFree:/ {print $2}' "$WORK/meminfo.out")
[ -n "$free_" ] && [ "$free_" -le "$total" ] && [ "$free_" -gt 0 ]
check "  and MemFree is below it" $?

# What filesystem the disk holds: the host made it.
[ "$(head -1 "$WORK/mounts.out")" = "/dev/hda / ext2 rw 0 0" ]
check "/proc/mounts says the root is the ext2 volume the host made" $?

grep -qE '^[0-9a-f]{8}-[0-9a-f]{8} r-xp 00000000 03:01 [0-9]+ +/bin/cat$' \
    "$WORK/maps.out"
check "cat's own maps: its text, named, with an inode" $?
grep -qE ' +\[stack\]$' "$WORK/maps.out"
check "  and its [stack]" $?
grep -qE ' +/lib/ld\.so$' "$WORK/maps.out"
check "  and the interpreter that loaded it" $?

# cat's stat: its own comm, and a parent that is not itself.
read -r spid scomm sstate sppid _ < "$WORK/stat.out"
[ "$scomm" = "(cat)" ] && [ "$sstate" = "R" ] && [ "$spid" != "$sppid" ]
check "cat /proc/self/stat is cat's, running, with a parent" $?
[ "$(wc -w < "$WORK/stat.out")" -eq 52 ]
check "  in fifty-two fields" $?

grep -qE '^lrwxrwxrwx .* exe -> /bin/ls$' "$WORK/lsl.out"
check "ls -l /proc/self/ shows exe as a link to ls" $?
grep -qE '^dr-x------ .* fd$' "$WORK/lsl.out"
check "  and fd/ as a directory only its owner may enter" $?

echo "=== the shell's own ls ==="
tr -s ' ' '\n' < "$WORK/bls.out" | grep -qx meminfo && \
    tr -s ' ' '\n' < "$WORK/bls.out" | grep -qx self
check "the shell's ls lists /proc -- it used to chdir, and could not" $?
grep -qE '^-r--r--r-- .* cmdline$' "$WORK/blsl.out" && \
    grep -qE '^dr-x------ .* fd$' "$WORK/blsl.out"
check "  and ls -l of a process shows its files and its fd/" $?

tr -d '\r' < "$LOG" | grep -qE 'panic|FPSP:|bus error|address error'
[ $? -ne 0 ]
check "no panic and no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
