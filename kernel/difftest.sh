#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# difftest.sh - GNU diffutils and GNU patch on the machine.
#
# A diff that only the same machine's patch has read proves nothing
# about either: they could agree on a format nobody else writes. So the
# two cross over with the HOST's tools both ways --
#
#   the machine's diff -u  ->  applied by the host's patch
#   the host's diff -u     ->  applied by the machine's patch
#
# -- and every result is compared on the host, byte for byte, with the
# file it should have produced. Then exit statuses (0 same, 1 differ,
# 2 trouble, which scripts and make depend on), a recursive diff, a
# binary file, diff3's merge, cmp, and patch -R, --dry-run, -p1 and a
# hunk that has moved.

set -u
# A write to the console FIFO after QEMU has died raises SIGPIPE, and
# that killed the suite before it printed a single check -- a machine
# that crashed read as silence. Ignored, the write fails and the checks
# say what went wrong.
trap '' PIPE
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-diff.img"; OFF=$((2048*512))
fsimg(){ PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" || {
    echo "difftest: fsimg $* failed" >&2; exit 1; }; }
LOG="$SCRATCH/difftest.log"; FIFO="$SCRATCH/diff.fifo"; T="$SCRATCH/diff.tmp"
rm -f "$LOG" "$FIFO"; rm -rf "$T"; mkdir -p "$T"

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1
../ports/diffutils/build.sh >/dev/null || exit 1
../ports/patch/build.sh >/dev/null || exit 1
DU=$SRCDIR/build-diffutils-sage040/sage040/bin
PA=$SRCDIR/build-patch-sage040/sage040/bin

# --- the files ------------------------------------------------------
seq 1 60 | sed 's/^/line /' > "$T/a"
# b: a changed line, a deleted one, two added, and one at the end.
sed -e 's/^line 10$/line ten/' -e '/^line 30$/d' -e 's/^line 45$/line 45\nnew 45a\nnew 45b/' \
    "$T/a" > "$T/b"; echo "the end" >> "$T/b"
# The host's diff of the same two, for the machine's patch to apply.
(cd "$T" && diff -u a b > host.diff); [ $? -le 1 ] || exit 1
# A copy of a with ten lines added at the top: the hunks have moved,
# and patch must find them by offset.
{ seq 1 10 | sed 's/^/top /'; cat "$T/a"; } > "$T/moved"
{ seq 1 10 | sed 's/^/top /'; cat "$T/b"; } > "$T/moved.want"
# diff3: base, mine (one change near the top), yours (one near the end).
printf 'one\ntwo\nthree\nfour\nfive\nsix\n' > "$T/base"
printf 'one\nTWO\nthree\nfour\nfive\nsix\n' > "$T/mine"
printf 'one\ntwo\nthree\nfour\nfive\nSIX\n' > "$T/yours"
printf 'one\nTWO\nthree\nfour\nfive\nSIX\n' > "$T/merged.want"
# Directories for diff -r, and a -p1 patch made from them.
mkdir -p "$T/d1/sub" "$T/d2/sub"
echo same > "$T/d1/same"; echo same > "$T/d2/same"
echo old > "$T/d1/sub/f"; echo new > "$T/d2/sub/f"
echo only > "$T/d2/only2"
(cd "$T" && diff -ruN d1 d2 > dirs.diff); [ $? -le 1 ] || exit 1
head -c 300 /dev/urandom > "$T/bin1"; cp "$T/bin1" "$T/bin2"; printf 'X' >> "$T/bin2"

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib; fsimg mkdir /usr; fsimg mkdir /usr/bin; fsimg mkdir /t
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
put_shells
for p in cat cp echo ls rm; do fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p; done
for p in diff cmp diff3 sdiff; do fsimg put -m 755 "$DU/$p" /usr/bin/$p; done
fsimg put -m 755 "$PA/patch" /usr/bin/patch
for f in a b host.diff moved base mine yours bin1 bin2 dirs.diff; do
    fsimg put "$T/$f" "/t/$f"
done
fsimg mkdir /t/d1; fsimg mkdir /t/d1/sub; fsimg mkdir /t/d2; fsimg mkdir /t/d2/sub
for f in same sub/f; do fsimg put "$T/d1/$f" "/t/d1/$f"; done
for f in same sub/f only2; do fsimg put "$T/d2/$f" "/t/d2/$f"; done
fsimg mkdir /t/p1; fsimg mkdir /t/p1/d1; fsimg mkdir /t/p1/d1/sub
for f in same sub/f; do fsimg put "$T/d1/$f" "/t/p1/d1/$f"; done

mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
pid=$!
exec 3> "$FIFO"
wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}
run() {                         # run TAG COMMAND
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" 600 || echo "    (timed out: $1)"
}

sleep 5
run SAME   '/usr/bin/diff /t/a /t/a'
run DIFF   '/usr/bin/diff -u /t/a /t/b > /t/mach.diff'
run TROUB  '/usr/bin/diff /t/a /t/no-such-file'
run HP     '/bin/cp /t/a /t/a2; /usr/bin/patch /t/a2 /t/host.diff'
run MOVED  '/usr/bin/patch /t/moved /t/host.diff'
run DRY    '/bin/cp /t/a /t/a3; /usr/bin/patch --dry-run /t/a3 /t/host.diff'
run REV    '/bin/cp /t/b /t/b2; /usr/bin/patch -R /t/b2 /t/host.diff'
run REC    '/usr/bin/diff -r /t/d1 /t/d2'
run BIN    '/usr/bin/diff /t/bin1 /t/bin2'
run CMP    '/usr/bin/cmp /t/bin1 /t/bin2'
run D3     '/usr/bin/diff3 -m /t/mine /t/base /t/yours > /t/merged'
run SDIFF  '/usr/bin/sdiff -s -w 40 /t/base /t/mine'
run P1     'cd /t/p1'
run P1RUN  '/usr/bin/patch -p1 -d d1 -i /t/dirs.diff'
run P1BACK 'cd /'
printf 'echo ALL-DONE\r' >&3
wait_for 'ALL-DONE' 100
printf 'halt\r' >&3
sleep 2
exec 3>&-; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
rm -f "$FIFO"
CLEAN="$SCRATCH/difftest-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[?0-9;]*[a-zA-Z]//g' > "$CLEAN"

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}
st() { sed -n "s/^$1-END //p" "$CLEAN" | head -1; }
get() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" get "$1" "$2" >/dev/null 2>&1; }

echo "=== checks: diff ==="
[ "$(st SAME)" = 0 ]
check "diff of a file with itself: exit 0" $?
[ "$(st DIFF)" = 1 ]
check "diff of two that differ: exit 1" $?
[ "$(st TROUB)" = 2 ]
check "diff of a file that is not there: exit 2" $?
get /t/mach.diff "$T/mach.diff"
cp "$T/a" "$T/host-applied"
patch -s "$T/host-applied" "$T/mach.diff" >/dev/null 2>&1 && cmp -s "$T/host-applied" "$T/b"
check "the machine's diff -u, applied by the HOST's patch, makes b" $?
grep -q '^@@ -7,7 +7,7 @@' "$T/mach.diff" && grep -q '^--- /t/a' "$T/mach.diff"
check "  and it is a unified diff, hunk headers and all" $?
[ "$(st REC)" = 1 ] && grep -aqx 'Only in /t/d2: only2' "$CLEAN" \
    && grep -aqx 'diff -r /t/d1/sub/f /t/d2/sub/f' "$CLEAN"
check "diff -r walks both trees: the file only in one, the one that changed" $?
grep -aqx 'Binary files /t/bin1 and /t/bin2 differ' "$CLEAN" && [ "$(st BIN)" = 1 ]
check "a binary file is reported, not dumped" $?
[ "$(st CMP)" = 1 ] && grep -aqE "^cmp: EOF on '/t/bin1' after byte 300" "$CLEAN"
check "cmp: one file is the start of the other, said so, exit 1" $?
get /t/merged "$T/merged"
[ "$(st D3)" = 0 ] && cmp -s "$T/merged" "$T/merged.want"
check "diff3 -m merges a change from each side with no conflict" $?
grep -aqP '^two\t+ *\|\tTWO$' "$CLEAN"     # sdiff pads with tabs
check "sdiff -s shows the one changed line side by side" $?

echo "=== checks: patch ==="
get /t/a2 "$T/a2"
[ "$(st HP)" = 0 ] && cmp -s "$T/a2" "$T/b"
check "the HOST's diff -u, applied by the machine's patch, makes b" $?
get /t/moved "$T/moved.got"
[ "$(st MOVED)" = 0 ] && cmp -s "$T/moved.got" "$T/moved.want" \
    && grep -aq 'offset 10 lines' "$CLEAN"
check "a hunk ten lines from where it was is found, and said to be" $?
get /t/a3 "$T/a3"
[ "$(st DRY)" = 0 ] && cmp -s "$T/a3" "$T/a"
check "--dry-run says it would apply, and changes nothing" $?
get /t/b2 "$T/b2"
[ "$(st REV)" = 0 ] && cmp -s "$T/b2" "$T/a"
check "-R takes the change back out" $?
get /t/p1/d1/sub/f "$T/p1f"; get /t/p1/d1/only2 "$T/p1only"
[ "$(st P1RUN)" = 0 ] && [ "$(cat "$T/p1f")" = new ] && [ "$(cat "$T/p1only")" = only ]
check "-p1 -d: a tree's patch, a changed file and a new one" $?

grep -aqE 'panic|bus error|address error|DOUBLE' "$CLEAN"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"; exit 1
