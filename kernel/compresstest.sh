#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# compresstest.sh - gzip, bzip2, xz and zstd, across two machines.
#
# A compressor that round-trips its own output can still be writing a
# format nobody else reads -- a byte-order slip in a header field, a
# checksum computed the wrong way round on a big-endian CPU -- and
# reading back what it wrote cannot show that. So each one is checked
# both ways against the HOST's own tools: the machine compresses, the
# host decompresses and compares; the host compresses, the machine
# decompresses and compares. The corpus is text, noise, zeroes and an
# empty file -- the cases the formats treat differently.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-compress.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/compresstest.log"; FIFO="$SCRATCH/compress.fifo"; WORK="$SCRATCH/compress.tmp"
rm -rf "$LOG" "$WORK"; mkdir -p "$WORK/in" "$WORK/fromhost" "$WORK/back"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "compresstest: fsimg $* failed" >&2; exit 1; }; }
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
TOOLS="gzip bzip2 xz zstd"
declare -A EXT=([gzip]=gz [bzip2]=bz2 [xz]=xz [zstd]=zst)

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
for t in $TOOLS; do ../ports/$t/build.sh >/dev/null || exit 1; done
for t in $TOOLS; do command -v $t >/dev/null || { echo "compresstest: no $t on the host" >&2; exit 1; }; done

# The corpus.
: > "$WORK/in/empty"
head -c 300000 /dev/urandom > "$WORK/in/noise"
head -c 300000 /dev/zero > "$WORK/in/zeroes"
cat ../os.md ../README.md > "$WORK/in/text"
CORPUS="empty noise zeroes text"
# The host's compressed copies, for the machine to undo.
for t in $TOOLS; do
    for f in $CORPUS; do $t -c "$WORK/in/$f" > "$WORK/fromhost/$f.${EXT[$t]}"; done
done

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=32 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /tmp /C /C/in /C/out /C/host /C/back; do fsimg mkdir $d; done
put_shells bash
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "$SAGE_LIBC/lib/libc.so" /lib/libc.so
for t in $TOOLS; do fsimg put -m 755 "$SRCDIR/build-$t-sage040/sage040/bin/$t" /bin/$t; done
fsimg put -m 755 ../ports/sbase/bin/cmp /bin/cmp
fsimg put "$WORK"/in/* /C/in/
fsimg put "$WORK"/fromhost/* /C/host/

# On the machine: compress the corpus, and undo the host's.
cat > "$WORK/run.sh" <<SCRIPT
for t in $TOOLS; do
    case \$t in gzip) e=gz ;; bzip2) e=bz2 ;; xz) e=xz ;; zstd) e=zst ;; esac
    for f in $CORPUS; do
        \$t -c /C/in/\$f > /C/out/\$f.\$e || echo "COMPRESS-FAIL \$t \$f"
        \$t -dc /C/host/\$f.\$e > /C/back/\$t-\$f || echo "DECOMPRESS-FAIL \$t \$f"
        cmp -s /C/in/\$f /C/back/\$t-\$f && echo "UNDO-OK \$t \$f" || echo "UNDO-BAD \$t \$f"
    done
done
echo COMPRESS-DONE
SCRIPT
fsimg put "$WORK/run.sh" /C/run.sh

rm -f "$FIFO"; mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide -display none -no-reboot -nic none \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3> "$FIFO"
wait_for() {
    local i
    for i in $(seq 1 "${2:-900}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -acxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qpid" 2>/dev/null || return 1
        sleep 0.5
    done
    return 1
}
wait_for 'kernel ready.*' 300
sleep 0.5
printf '/bin/bash /C/run.sh; echo "RUN-END $?"\r' >&3
wait_for 'RUN-END [0-9]+' 7200
printf 'sync\r' >&3
sleep 3
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

CL="$WORK/clean"; tr -d '\r' < "$LOG" > "$CL"
mkdir -p "$WORK/out"
for t in $TOOLS; do
    for f in $CORPUS; do
        PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" get "/C/out/$f.${EXT[$t]}" "$WORK/out/$f.${EXT[$t]}" >/dev/null 2>&1
    done
done

echo "=== checks ==="
grep -qx 'COMPRESS-DONE' "$CL"; check "the machine ran every compression" $?
for t in $TOOLS; do
    ok=0; bad=""
    for f in $CORPUS; do
        $t -dc "$WORK/out/$f.${EXT[$t]}" 2>/dev/null | cmp -s - "$WORK/in/$f" || { ok=1; bad="$bad $f"; }
    done
    check "$t: what the machine wrote, the host's $t reads back exactly${bad:+ (not:$bad)}" $ok
    n=$(grep -c "^UNDO-OK $t " "$CL")
    [ "$n" = "$(echo $CORPUS | wc -w)" ]
    check "  and what the host's $t wrote, the machine reads back exactly ($n of $(echo $CORPUS | wc -w))" $?
done
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
