#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# pylibtest.sh - the libraries CPython is built against, proven by the
# programs that come with them.
#
# bzip2, xz, zstd, sqlite and OpenSSL are here because CPython's bz2,
# lzma, compression.zstd, sqlite3, ssl and hashlib are each nothing at
# all without them. Each one also ships a program, and that program is
# how the library gets tested without Python in the way: if the machine
# can compress a file the HOST can decompress, the library works.
#
# WHY EVERY CHECK CROSSES THE HOST BOUNDARY. A compressor tested by
# decompressing its own output proves almost nothing -- t3-ata is this
# project's standing example, where a byte-swapped disk passed a
# write-then-read-back test for weeks because both directions swapped.
# These formats are byte streams with defined byte order, and this is a
# BIG-ENDIAN machine, which is exactly the case where an implementation
# can be self-consistently wrong. So every stream goes both ways: the
# guest compresses and the host decompresses, and the host compresses
# and the guest decompresses. A mistake in either direction shows.
#
# The digests are the same argument in its sharpest form: a hash is a
# number derived from bytes in a defined order, and a big-endian
# implementation that got that order wrong would be perfectly
# self-consistent and completely wrong. So the expected values come
# from coreutils' sha256sum and friends -- a separate implementation on
# a different CPU -- and from the published vector for the empty
# string, which is the same on every machine ever built.
#
# WHAT THESE CHECKS DO NOT PROVE. ports/openssl/build.sh passes
# -DB_ENDIAN, and it is tempting to read a pass here as proof that the
# flag is needed. It is not: OpenSSL was built a second time without
# it and answers identically. These checks say the digests are right,
# which is the question worth asking; they say nothing about that
# flag.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-pylib.img"
PART_LBA=2048
MIMG="$DISK@@$((PART_LBA * 512))"
LOG="$SCRATCH/pylibtest.log"
WORK="$SCRATCH/pylib.tmp"
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
skip() {
    echo "  [SKIP] $1"
}

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
BZOUT=$SRCDIR/build-bzip2-sage040/sage040
XZOUT=$SRCDIR/build-xz-sage040/sage040
ZSTDOUT=$SRCDIR/build-zstd-sage040/sage040
SQLOUT=$SRCDIR/build-sqlite-sage040/sage040
SSLOUT=$SRCDIR/build-openssl-sage040/sage040
FFIOUT=$SRCDIR/build-libffi-sage040/sage040

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../ldso || exit 1
for p in bzip2 xz zstd sqlite openssl libffi; do
    ../ports/$p/build.sh > /dev/null 2>&1 || {
        echo "pylibtest: ports/$p/build.sh failed" >&2; exit 1; }
done

# libffi has no program of its own, so it gets one: ports/libffi/test.
# It is built here rather than by the port, because it is a test and
# nothing else links it.
. ../ports/cross.sh
# shellcheck disable=SC2086
"$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS -I"$FFIOUT/include" \
    $DYN_LDFLAGS ../ports/libffi/test/fficheck.c "$FFIOUT/lib/libffi.a" \
    $DYN_LIBS -o "$SCRATCH/fficheck" || {
    echo "pylibtest: fficheck would not build" >&2; exit 1; }

# The host tools each check needs. Missing ones turn checks into skips
# rather than into passes -- a check that cannot run must never look
# like a check that ran.
have() { command -v "$1" > /dev/null 2>&1; }

echo "=== the test data ==="
# Deterministic, and deliberately NOT random: a mixture of long runs
# (which every one of these formats encodes specially), text, and every
# byte value, so a decoder that mishandles a literal run, a match or a
# high byte has something to get wrong. 64 KB is past every one of
# their block boundaries.
python3 - "$WORK/data.bin" <<'PY'
import sys
out = bytearray()
out += b"The quick brown fox jumps over the lazy dog.\n" * 200
out += bytes(range(256)) * 64
out += b"\x00" * 4096
out += b"".join(bytes([(i * 7 + 13) & 0xFF]) for i in range(16384))
out += ("SuckOS on a 68040, big-endian, %d bytes so far.\n" % len(out)).encode()
open(sys.argv[1], "wb").write(bytes(out))
PY
ls -l "$WORK/data.bin" | awk '{print "    data.bin is " $5 " bytes"}'

# The host's compressed copies, for the guest to decompress.
bzip2  -9 -c "$WORK/data.bin" > "$WORK/host.bz2"
# XZ AT -1, NOT AT ITS DEFAULT, and that is a fact about this machine
# rather than a preference. LZMA's memory use is set by the dictionary
# size: -6, which is what plain `xz` uses, wants about 94 MB to
# compress and this machine has 64 MB of RAM in total. It does not
# crash -- it says "Not enough space" and exits 1 -- but it cannot run.
# -1 needs about 9 MB. The check below records that the default really
# does fail, so this line is not mistaken for timidity.
xz     -1 -c "$WORK/data.bin" > "$WORK/host.xz"
zstd   -19 -q -c "$WORK/data.bin" > "$WORK/host.zst"

# The host's digests, from coreutils -- a different implementation from
# OpenSSL's, which is the point.
exp_md5=$(md5sum    "$WORK/data.bin" | cut -d' ' -f1)
exp_sha1=$(sha1sum  "$WORK/data.bin" | cut -d' ' -f1)
exp_sha256=$(sha256sum "$WORK/data.bin" | cut -d' ' -f1)
exp_sha512=$(sha512sum "$WORK/data.bin" | cut -d' ' -f1)

# A host-made database for the guest to read.
if have sqlite3; then
    rm -f "$WORK/host.db"
    sqlite3 "$WORK/host.db" \
        "create table planets(name text, moons integer);
         insert into planets values('mars',2),('jupiter',95),('earth',1);" \
        > /dev/null
fi

# The SQL the guest runs, as a file: the machine's shell would have to
# carry a multi-line quoted argument otherwise.
cat > "$WORK/make.sql" <<'EOF'
create table t(n integer, s text);
insert into t values(1,'one');
insert into t values(2,'two');
insert into t values(3,'three');
create table big(v integer);
insert into big select 1;
EOF
cat > "$WORK/read.sql" <<'EOF'
.mode list
.separator |
select name, moons from planets order by moons desc;
EOF

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (64 * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mmd -i "$MIMG" ::/BIN ::/lib ::/ST
mcopy -o -i "$MIMG" ../system/sh ::/BIN/sh
mcopy -o -i "$MIMG" ../ldso/ld.so ::/lib/ld.so
mcopy -o -i "$MIMG" "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" ::/lib/libc.so
mcopy -o -i "$MIMG" "$BZOUT/bin/bzip2"    ::/BIN/bzip2
mcopy -o -i "$MIMG" "$XZOUT/bin/xz"       ::/BIN/xz
mcopy -o -i "$MIMG" "$ZSTDOUT/bin/zstd"   ::/BIN/zstd
mcopy -o -i "$MIMG" "$SQLOUT/bin/sqlite3" ::/BIN/sqlite3
[ -x "$SSLOUT/bin/openssl" ] && \
    mcopy -o -i "$MIMG" "$SSLOUT/bin/openssl" ::/BIN/openssl
mcopy -o -i "$MIMG" "$SCRATCH/fficheck" ::/BIN/fficheck
mcopy -o -i "$MIMG" "$WORK/data.bin" ::/ST/data.bin
mcopy -o -i "$MIMG" "$WORK/host.bz2" ::/ST/host.bz2
mcopy -o -i "$MIMG" "$WORK/host.xz"  ::/ST/host.xz
mcopy -o -i "$MIMG" "$WORK/host.zst" ::/ST/host.zst
mcopy -o -i "$MIMG" "$WORK/make.sql" ::/ST/make.sql
mcopy -o -i "$MIMG" "$WORK/read.sql" ::/ST/read.sql
[ -f "$WORK/host.db" ] && mcopy -o -i "$MIMG" "$WORK/host.db" ::/ST/host.db

rm -f "$SCRATCH/pylib.fifo"
mkfifo "$SCRATCH/pylib.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/pylib.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/pylib.fifo"

wait_for() {
    for _ in $(seq 1 ${2:-1800}); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxF -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() {                         # run COMMAND TAG
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2" || echo "    (timed out waiting for $2)"
    sleep 0.1
}

sleep "$BOOT_WAIT"
run 'cd /ST' cd

echo "=== running on the machine (this takes a few minutes) ==="

# Each compressor, both directions.
run 'bzip2 -c data.bin > guest.bz2'      bz-c
run 'bzip2 -d -c host.bz2 > from-host.bz.bin' bz-d
run 'xz -1 -c data.bin > guest.xz'       xz-c
run 'xz -d -c host.xz > from-host.xz.bin'     xz-d
# The default preset, which this machine cannot afford: it must fail
# cleanly and say so, not crash and not silently produce a short file.
run 'xz -c data.bin > toobig.xz' xz-default
run 'zstd -q -c data.bin > guest.zst'    zst-c
run 'zstd -q -d -c host.zst > from-host.zst.bin' zst-d

# SQLite: a database the machine builds, and one it reads.
run 'sqlite3 guest.db < make.sql'        sql-make
# THE SEPARATOR HAS TO BE QUOTED. The shell has pipelines now, so a
# bare `-separator |` split the command in two and sqlite3 reported a
# missing argument -- which the check then read as a broken database.
run 'sqlite3 -separator "|" guest.db "select n,s from t order by n" > sql-sel.out' sql-sel
run 'sqlite3 host.db < read.sql > sql-host.out' sql-host
run 'sqlite3 -version > sql-ver.out'     sql-ver

# OpenSSL: digests, and a cipher.
run 'openssl dgst -md5 -r data.bin > md5.out'       ssl-md5
run 'openssl dgst -sha1 -r data.bin > sha1.out'     ssl-sha1
run 'openssl dgst -sha256 -r data.bin > sha256.out' ssl-sha256
run 'openssl dgst -sha512 -r data.bin > sha512.out' ssl-sha512
run 'openssl dgst -sha256 -r /dev/null > empty.out' ssl-empty
run 'openssl enc -aes-256-cbc -K 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f -iv 000102030405060708090a0b0c0d0e0f -in data.bin -out guest.aes' ssl-enc
run 'openssl version > ssl-ver.out'      ssl-ver

# libffi: calls built from a description at run time, and a closure --
# code libffi writes into memory and then hands out as an ordinary C
# function pointer. Its own output goes to the console so the reasons
# are visible in the session, and its verdict is read from there.
run 'fficheck'                           ffi

run 'echo ALL-DONE' end
wait_for "ALL-DONE"
sleep 0.5

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/pylib.fifo"
tr -d '\r' < "$LOG" > "$WORK/session.txt"

echo "=== guest session (tail) ==="
sed 's/^/  | /' "$WORK/session.txt" | tail -25

# Everything the machine wrote, taken off the disk with the HOST's
# mtools rather than read back by the machine itself.
get() { mcopy -n -o -i "$MIMG" "::/ST/$1" "$WORK/$1" 2>/dev/null; }
for f in guest.bz2 from-host.bz.bin guest.xz from-host.xz.bin toobig.xz \
         guest.zst from-host.zst.bin guest.db sql-sel.out sql-host.out \
         sql-ver.out md5.out sha1.out sha256.out sha512.out empty.out \
         guest.aes ssl-ver.out; do
    get "$f"
done

echo "=== checks ==="

! grep -qE "panic|exception at|DOUBLE MMU" "$WORK/session.txt"
check "no panic, no kernel exception" $?

# --- the compressors -------------------------------------------------
for fmt in bz2:bzip2:bz xz:xz:xz zst:zstd:zst; do
    ext=${fmt%%:*}; rest=${fmt#*:}; tool=${rest%%:*}; tag=${rest##*:}

    if [ -s "$WORK/guest.$ext" ]; then
        "$tool" -d -c "$WORK/guest.$ext" > "$WORK/host-decoded.$ext.bin" 2>/dev/null
        cmp -s "$WORK/host-decoded.$ext.bin" "$WORK/data.bin"
        check "$tool: the HOST decompresses what the machine compressed" $?
    else
        check "$tool: the machine produced a compressed file" 1
    fi

    cmp -s "$WORK/from-host.$tag.bin" "$WORK/data.bin"
    check "  and the machine decompresses what the HOST compressed" $?
done

# xz at its default preset asks for more memory than the machine has.
# It must refuse rather than crash or truncate: the session says so,
# and the file it did not manage to write must not be a valid stream.
grep -q "Not enough space" "$WORK/session.txt"
check "xz: the default preset refuses cleanly (64 MB is not enough)" $?
! xz -t "$WORK/toobig.xz" 2>/dev/null
check "  and left nothing that pretends to be a compressed file" $?

# The stream the machine produced must not merely decode -- it must be
# a DIFFERENT file from the input, or a compressor that copied its
# input would pass everything above.
test -s "$WORK/guest.bz2" && \
    ! cmp -s "$WORK/guest.bz2" "$WORK/data.bin"
check "the compressed stream is not just a copy of the input" $?

# --- SQLite ----------------------------------------------------------
if have sqlite3 && [ -s "$WORK/guest.db" ]; then
    got=$(sqlite3 -separator '|' "$WORK/guest.db" \
              "select n,s from t order by n" 2>/dev/null | tr '\n' ' ')
    [ "$got" = "1|one 2|two 3|three " ]
    check "sqlite: the HOST reads the database the machine built" $?

    sqlite3 "$WORK/guest.db" "pragma integrity_check" 2>/dev/null \
        | grep -qx "ok"
    check "  and sqlite's own integrity check passes on it" $?
elif ! have sqlite3; then
    skip "sqlite: the host has no sqlite3 to read the machine's database"
    test -s "$WORK/guest.db"
    check "  the machine built a database at all" $?
else
    check "sqlite: the machine built a database" 1
    skip "  sqlite's integrity check (there is no database to check)"
fi

tr -d ' ' < "$WORK/sql-sel.out" 2>/dev/null | tr '\n' ' ' \
    | grep -q "1|one 2|two 3|three"
check "  the machine reads back its own rows" $?

if [ -s "$WORK/host.db" ]; then
    tr -d '\r' < "$WORK/sql-host.out" 2>/dev/null | tr '\n' ' ' \
        | grep -q "jupiter|95 mars|2 earth|1"
    check "  and reads a database the HOST built, in the right order" $?
else
    skip "  reading a host-built database (no host sqlite3)"
fi

grep -q "^3\." "$WORK/sql-ver.out" 2>/dev/null
check "  sqlite3 -version answers" $?

# --- OpenSSL ---------------------------------------------------------
digest() { cut -d' ' -f1 < "$WORK/$1" 2>/dev/null | tr -d '\r\n *'; }

if [ -s "$WORK/sha256.out" ]; then
    [ "$(digest md5.out)" = "$exp_md5" ]
    check "openssl: MD5 agrees with the host's md5sum" $?
    [ "$(digest sha1.out)" = "$exp_sha1" ]
    check "  SHA-1 agrees with the host's sha1sum" $?
    [ "$(digest sha256.out)" = "$exp_sha256" ]
    check "  SHA-256 agrees with the host's sha256sum" $?
    [ "$(digest sha512.out)" = "$exp_sha512" ]
    check "  SHA-512 agrees with the host's sha512sum" $?

    # The published SHA-256 of the empty string: the same on every
    # machine that has ever computed it, and owed to nothing on this
    # host at all.
    [ "$(digest empty.out)" = \
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ]
    check "  and the empty string's SHA-256 is the published value" $?
else
    check "openssl: the machine computed a digest" 1
fi

if [ -s "$WORK/guest.aes" ] && have openssl; then
    openssl enc -d -aes-256-cbc \
        -K 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f \
        -iv 000102030405060708090a0b0c0d0e0f \
        -in "$WORK/guest.aes" -out "$WORK/aes-plain.bin" 2>/dev/null
    cmp -s "$WORK/aes-plain.bin" "$WORK/data.bin"
    check "  the HOST decrypts what the machine encrypted (AES-256-CBC)" $?
else
    check "  the machine encrypted a file" 1
fi

grep -q "^OpenSSL 3\." "$WORK/ssl-ver.out" 2>/dev/null
check "  openssl version answers" $?

# --- libffi ----------------------------------------------------------
#
# fficheck prints its own eleven reasons into the session; what is
# checked here is its verdict and that every one of them ran. A suite
# that only looked for "PASS" would be satisfied by a program that
# printed it and did nothing.
grep -q "^FFI-RESULT: PASS" "$WORK/session.txt"
check "libffi: calls and closures work (fficheck)" $?
ffiok=$(grep -c "^  \[ OK \]" "$WORK/session.txt")
test "$ffiok" -eq 11
check "  and all eleven of its checks ran ($ffiok)" $?
! grep -q "^  \[FAIL\]" "$WORK/session.txt"
check "  with none of them failing" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
