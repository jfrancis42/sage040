#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# sbasetest.sh - the small utilities (sbase), on the machine (task 29).
#
# ports/sbase/tests/cases.txt: each line is a command run in the test
# directory on the machine, its output and status kept in a file, and
# the same command run on the host with its own (GNU) tools. The two are
# compared on the host -- sort orders, checksums, od dumps and CRCs are
# answers two different implementations must agree on. A '~' compares
# with whitespace squeezed, where column widths may fairly differ; a '@'
# compares the lines SORTED, for output whose order is a directory's
# readdir order -- neither find promises one, so comparing it byte for
# byte would be a test of the two filesystems' entry order and not of
# the utility.
#
# Then what only the disk can say: a tar archive the host's tar reads, a
# dd whose size the host reports, a date touch set, make rebuilding a file
# only when its source is newer, ed editing a file in place, directories
# made and removed.
#
# Utilities with a shell builtin of the same name (ls, cp, echo, test...)
# are reached as /bin/NAME, as a script or xargs reaches them.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-sbase.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/sbasetest.log"
WORK="$SCRATCH/sbase.tmp"
rm -f "$LOG"
rm -rf "$WORK"
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
../ports/sbase/build.sh >/dev/null || exit 1

T=../ports/sbase/tests
mkdir -p "$WORK/host" "$WORK/got" "$WORK/expect"
cp "$T"/* "$WORK/host/"

# The host's answers.
while IFS='|' read -r tag cmd; do
    case "$tag" in ''|'#'*) continue ;; esac
    cmd=${cmd#\~}
    cmd=${cmd#=}
    cmd=${cmd#@}
    (cd "$WORK/host" && LC_ALL=C bash -c "$cmd" > "$WORK/expect/$tag.OUT" 2>&1
     echo "status $?" >> "$WORK/expect/$tag.OUT")
done < "$T/cases.txt"

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /ST; fsimg mkdir /tmp
fsimg put -m 755 ../system/sh /bin/sh
fsimg put -m 755 ../ports/sbase/bin/* /bin/
fsimg mkdir /share; fsimg mkdir /share/misc
fsimg put -m 755 ../ports/sbase/share/misc/bc.library /share/misc/
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put "$T"/* /ST/

rm -f "$SCRATCH/sbase.fifo"
mkfifo "$SCRATCH/sbase.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/sbase.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/sbase.fifo"

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
run 'cd /ST' cd
while IFS='|' read -r tag cmd; do
    case "$tag" in ''|'#'*) continue ;; esac
    cmd=${cmd#\~}
    cmd=${cmd#=}
    cmd=${cmd#@}
    run "$cmd > $tag.OUT 2>&1; echo status \$? >> $tag.OUT" "c-$tag"
done < "$T/cases.txt"

# --- what the disk says ------------------------------------------------
run 'tar -cf arch.tar c1.txt c2.txt fruit.txt' tar
run 'dd if=/dev/zero of=zero.bin bs=512 count=4' dd
run 'touch -d 2001-02-03T04:05:06 touched.txt' touch
run 'make > make1.out 2>&1' make1
run 'make > make2.out 2>&1' make2
run 'sleep 3' nap
run 'printf newer > in.txt' newer
run 'make > make3.out 2>&1' make3
run 'ed -s ed.txt < ed.cmd' ed
run '/bin/mkdir -p deep/er/est' mkdirp
run '/bin/cp -r deep copied' cpr
run '/bin/mv copied moved' mv
run '/bin/rm -r deep' rmr
run 'ln fruit.txt hard.txt > ln.out 2>&1; echo status $? >> ln.out' ln
run 'which sort > which.out' which
run '/bin/date -u +%Y > year.out' date
run 'uuencode c1.txt c1.txt > uu.enc' uuenc
run 'uudecode -o uu.out uu.enc' uudec
run 'export LANG=C.UTF-8' lang
run 'rev utf8.txt > revutf.out' revutf
run 'tail -m 3 utf8.txt > tailm.out' tailm
run '/bin/uname -s > uname.out' uname
run 'hostname > host.out' hostname
run 'mktemp > mktemp.out' mktemp
run 'halt' halt

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/sbase.fifo"

fsimg get -r /ST "$WORK/got" 2>/dev/null; mv "$WORK/got/ST"/* "$WORK/got/" 2>/dev/null
tr -d '\r' < "$LOG" > "$SCRATCH/sbase-clean.tmp"

echo "=== checks: each utility against the host's ==="
squeeze() { tr -s ' \t' ' ' < "$1" | sed 's/^ //; s/ $//'; }
values() {                      # every word, hex ones as numbers
    tr -s ' \t\n' '\n\n\n' < "$1" | grep -v '^$' |
        while read -r w; do
            case "$w" in *[!0-9a-fA-F]*) echo "$w" ;; *) echo $((16#$w)) ;; esac
        done
}
while IFS='|' read -r tag cmd; do
    case "$tag" in ''|'#'*) continue ;; esac
    if [ "${cmd#\~}" != "$cmd" ]; then
        cmp -s <(squeeze "$WORK/got/$tag.OUT") <(squeeze "$WORK/expect/$tag.OUT")
    elif [ "${cmd#=}" != "$cmd" ]; then
        cmp -s <(values "$WORK/got/$tag.OUT") <(values "$WORK/expect/$tag.OUT")
    elif [ "${cmd#@}" != "$cmd" ]; then
        cmp -s <(sort "$WORK/got/$tag.OUT") <(sort "$WORK/expect/$tag.OUT")
    else
        cmp -s "$WORK/got/$tag.OUT" "$WORK/expect/$tag.OUT"
    fi
    r=$?
    shown=${cmd#\~}
    shown=${shown#=}
    check "$tag: ${shown#@}" $r
    [ $r -ne 0 ] && diff "$WORK/expect/$tag.OUT" "$WORK/got/$tag.OUT" 2>&1 | head -6 | sed 's/^/        /'
done < "$T/cases.txt"

echo "=== checks: what the disk says ==="
G=$WORK/got
( mkdir -p "$WORK/untar" && cd "$WORK/untar" && tar -xf "$G/arch.tar" ) &&
    cmp -s "$WORK/untar/fruit.txt" "$T/fruit.txt" && cmp -s "$WORK/untar/c2.txt" "$T/c2.txt"
check "tar: an archive made on the machine, read by the host's tar" $?
[ "$(stat -c %s "$G/zero.bin" 2>/dev/null)" = 2048 ] && ! tr -d '\0' < "$G/zero.bin" | grep -q .
check "dd: 4 blocks of 512 zero bytes" $?
# The RAW seconds, not a printed date: debugfs prints its own format in
# the HOST's timezone, so matching the text tests where the host is.
# 981173106 is 2001-02-03 04:05:06 UTC.
[ "$(fsimg mtime /ST/touched.txt 2>/dev/null)" = 981173106 ]
check "touch -d: the time the host reads off the disk, to the second" $?
[ "$(cat "$G/out.txt" 2>/dev/null)" = newer ]
r=$?
grep -q "cp in.txt out.txt" "$G/make1.out" && ! grep -q "cp in.txt" "$G/make2.out" &&
    grep -q "cp in.txt out.txt" "$G/make3.out" && [ $r -eq 0 ]
check "make: builds, then nothing to do, then rebuilds once the source is newer" $?
[ "$(head -1 "$G/ed.txt")" = HELLO ] && grep -q "HELLO again" "$G/ed.txt"
check "ed: a script of commands edits the file in place" $?
[ -d "$G/moved/er/est" ] && [ ! -e "$G/deep" ] && [ ! -e "$G/copied" ]
check "mkdir -p, cp -r, mv, rm -r: the tree the host sees" $?
# ln WORKS now. This checked that it was refused and left nothing
# behind, which was true while the filesystem was FAT and had no link
# count. ext2 has one, so the link is made -- and the two names must be
# the SAME inode, not a copy, which is the thing worth checking.
grep -q "status 0" "$G/ln.out" && [ -e "$G/hard.txt" ]
check "ln: a second name is made" $?
# ON THE IMAGE, not on the copy fetched out of it: `fsimg get -r`
# writes two ordinary files for two names, so the host copy cannot
# show that they were one inode. The image can.
a=$(fsimg ls-l /ST 2>/dev/null | awk '$NF=="hard.txt"{print $1}')
b=$(fsimg ls-l /ST 2>/dev/null | awk '$NF=="fruit.txt"{print $1}')
[ -n "$a" ] && [ "$a" = "$b" ]
check "  and it is the same inode as the original, not a copy ($a)" $?
cmp -s "$G/uu.out" "$T/c1.txt"
check "uuencode and uudecode: a round trip" $?
[ "$(cat "$G/revutf.out")" = "$(printf '日éa')" ]
check "rev reverses characters, not bytes (upstream had it inverted)" $?
[ "$(cat "$G/tailm.out")" = "$(printf 'é日')" ]
check "tail -m counts characters (upstream had it inverted too)" $?
grep -qx "/bin/sort" "$G/which.out"
check "which finds /bin/sort" $?
[ "$(cat "$G/year.out")" = "$(date -u +%Y)" ]
check "date -u +%Y is this year" $?
[ -s "$G/uname.out" ] && [ -s "$G/host.out" ]
check "uname -s and hostname answer" $?
grep -q "^/tmp/" "$G/mktemp.out"
check "mktemp made a file in /tmp" $?

! grep -q "panic\|exception" "$SCRATCH/sbase-clean.tmp"
check "no panic, no kernel exception" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
