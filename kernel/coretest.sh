#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# coretest.sh - core files (kernel/coredump.c), and RLIMIT_CORE.
#
# libc/test/crashee dies on purpose, with a known value in a global
# and a known argument in the function it dies in. Its core is read
# twice, by two readers that share no code: the machine's own gdb,
# which must print the value, the backtrace and the signal; and Python
# on the host, which walks the ELF itself to find the value at the
# global's address (from `nm`) and the PC in the NT_PRSTATUS registers,
# which must be inside boom(). readelf checks the notes have exactly the
# sizes bfd recognises Linux/m68k's by.
#
# The limit decides: ulimit -c 0 (Linux's default) leaves no core, and
# a limit smaller than the core cuts it off there. SIGQUIT dumps,
# SIGTERM does not, and a waiting parent sees 0x80 (WCOREDUMP) exactly
# when a core was written.

set -u
cd "$(dirname "$0")"
. ../machine.conf
. ./shells.sh

QEMU=${QEMU:-$HOME/m68k/sage040-qemu/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k
SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}; mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-core.img"; OFF=$((2048 * 512))
LOG="$SCRATCH/coretest.log"; FIFO="$SCRATCH/core.fifo"
WORK="$SCRATCH/core.tmp"
rm -rf "$LOG" "$WORK"; mkdir -p "$WORK"
fsimg() { PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" "$@" ||
    { echo "coretest: fsimg $* failed" >&2; exit 1; }; }
SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
GDBOUT=$SRCDIR/build-gdb-sage040/sage040
M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}

pass=0; fail=0
check() {
    if [ "$2" -eq 0 ]; then echo "  [ OK ] $1"; pass=$((pass + 1))
    else echo "  [FAIL] $1"; fail=$((fail + 1)); fi
}

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../libc/test crashee >/dev/null 2>&1 || exit 1
../ports/sbase/build.sh >/dev/null 2>&1 || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=48 status=none
printf 'label: dos\nunit: sectors\nstart=2048, type=83\n' | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040 >/dev/null
fsimg put kernel.rom /KERNEL.ROM
for d in /bin /lib /usr /usr/bin /tmp /T0 /T1 /T2 /T3 /T4 /T5; do fsimg mkdir $d; done
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
put_shells bash
for p in cat sleep kill true ls; do fsimg put -m 755 "../ports/sbase/bin/$p" /bin/$p; done
fsimg put -m 755 "$GDBOUT/bin/gdb" /usr/bin/gdb
fsimg put -m 755 ../libc/test/crashee /crashee
fsimg put ../libc/test/crashee.c /crashee.c

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
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s; echo "%s"-END $?\r' "$2" "$1" >&3
    wait_for "$1-END [0-9]+" 3000 || echo "    (timed out: $1)"
}
b() { echo "/bin/bash -c '$1'"; }

wait_for 'kernel ready.*' 300
sleep 1
run START 'echo started'
run NOLIMIT "$(b 'cd /T0; /crashee segv; echo st=$?')"
run SEGV "$(b 'cd /T1; ulimit -c unlimited; /crashee segv; echo st=$?')"
run ABORT "$(b 'cd /T2; ulimit -c unlimited; /crashee wait abort')"
run QUIT "$(b 'cd /T3; ulimit -c unlimited; /crashee sleep & p=$!; sleep 2; kill -QUIT $p; wait $p; echo st=$?')"
run IGNORE "$(b '/crashee ignore & p=$!; sleep 1; kill -QUIT $p; wait $p; echo st=$?')"
run INHERIT "$(b '/crashee inherit & p=$!; sleep 1; kill -QUIT $p; wait $p; echo st=$?')"
run LONG "$(b '/crashee inherit 300 & p=$!; sleep 2; kill -QUIT $p; sleep 2; kill -0 $p && echo alive; kill -KILL $p; wait $p; echo st=$?')"
run TERM "$(b 'cd /T4; ulimit -c unlimited; /crashee sleep & p=$!; sleep 2; kill -TERM $p; wait $p; echo st=$?')"
run SMALL "$(b 'cd /T5; ulimit -c 2; /crashee wait segv')"
run NOCORE "$(b 'cd /T0; /crashee wait segv')"
run LIMITS "$(b 'ulimit -c; ulimit -Hc; ulimit -c 10; ulimit -c; ulimit -Hc 20; ulimit -Hc; ulimit -Hc 30; echo raise=$?')"
run LS '/bin/ls -l /T0 /T1 /T2 /T3 /T4 /T5'
run GDB "/usr/bin/gdb -batch -nx -ex 'print/x marker' -ex bt -ex 'print/x \$pc' /crashee /T1/core"
printf 'sync; halt\r' >&3
sleep 3
exec 3>&-
kill -9 "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
rm -f "$FIFO"

for d in T1 T2 T3 T5; do
    PART_OFFSET=$OFF ../tools/fsimg.sh "$DISK" get /$d/core "$WORK/$d.core" >/dev/null 2>&1
done

CL="$SCRATCH/core-clean.tmp"
tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\x1b\][^\x1b]*\x1b\\//g' > "$CL"
echo "=== guest session ==="
sed -n '/started/,$p' "$CL" | sed 's/^/  | /' | head -90
block() { sed -n "/echo \"$1\"-END/,/^$1-END/p" "$CL"; }

echo "=== checks ==="
lsT() { block LS | sed -n "/^\/$1:/,/^\$/p"; }
block NOLIMIT | grep -qx 'st=139' && ! lsT T0 | grep -q ' core$'
check "ulimit -c 0 (the default): SIGSEGV, status 139, and no core" $?
block SEGV | grep -qx 'st=139' && lsT T1 | grep -qE '^-rw------- .* core$'
check "ulimit -c unlimited: a core, mode 0600, in the working directory" $?
block ABORT | grep -qx 'signal=6 core=1'
check "abort(): the parent sees SIGABRT and WCOREDUMP (0x80)" $?
block QUIT | grep -qx 'st=131' && lsT T3 | grep -q ' core$'
check "kill -QUIT from outside: a core" $?
block IGNORE | grep -qx 'slept, 0 left' && block IGNORE | grep -qx 'st=0'
check "an IGNORED SIGQUIT does not cut a sleep short" $?
block INHERIT | grep -qx 'slept, 0 left' && block INHERIT | grep -qx 'st=0'
check "  nor one ignored by the shell for a background job, across exec" $?
block LONG | grep -qx 'alive' && block LONG | grep -qx 'st=137'
check "  nor a long one (300 s): still asleep after, until SIGKILL" $?
block TERM | grep -qx 'st=143' && ! lsT T4 | grep -q ' core$'
check "kill -TERM: ended, and no core -- SIGTERM's action is not 'core'" $?
sz=$(stat -c %s "$WORK/T5.core" 2>/dev/null || echo 0)
block SMALL | grep -qx 'signal=11 core=1' && [ "$sz" -gt 0 ] && [ "$sz" -le 2048 ]
check "ulimit -c 2: cut off at 2048 bytes ($sz), still reported as a core" $?
block NOCORE | grep -qx 'signal=11 core=0'
check "  and with no limit set, WCOREDUMP is clear" $?
[ "$(block LIMITS | grep -xE '[0-9]+|unlimited' | tr '\n' ' ')" = "0 unlimited 10 20 " ] &&
    block LIMITS | grep -qx 'raise=0'
check "getrlimit/setrlimit keep RLIMIT_CORE: 0, unlimited, 10, hard 20 (root may raise it again)" $?

# The host's own reading of the T1 core.
RE=$M68K_PREFIX/bin/m68k-elf-readelf; NM=$M68K_PREFIX/bin/m68k-elf-nm
"$RE" -h "$WORK/T1.core" 2>/dev/null | grep -q 'CORE (Core file)'
check "readelf: an ELF core file" $?
notes=$("$RE" -n "$WORK/T1.core" 2>/dev/null)
echo "$notes" | grep -qE 'CORE[[:space:]]+0x0000009a[[:space:]]+NT_PRSTATUS' &&
    echo "$notes" | grep -qE 'CORE[[:space:]]+0x0000006c[[:space:]]+NT_(FPREGSET|PRFPREG)' &&
    echo "$notes" | grep -qE 'CORE[[:space:]]+0x0000007c[[:space:]]+NT_PRPSINFO'
check "  with NT_PRSTATUS 154, NT_FPREGSET 108, NT_PRPSINFO 124 bytes" $?
MARK=$("$NM" ../libc/test/crashee | awk '$3 == "marker" {print $1}')
BOOM=$("$NM" -S ../libc/test/crashee | awk '$4 == "boom" {print $1, $2}')
out=$(python3 - "$WORK/T1.core" "$MARK" $BOOM <<'PY'
import struct, sys
core = open(sys.argv[1], 'rb').read()
mark, boom, size = (int(x, 16) for x in sys.argv[2:5])
phoff, = struct.unpack('>I', core[28:32]); phnum, = struct.unpack('>H', core[44:46])
val = pc = sig = None
for i in range(phnum):
    t, off, va, pa, fsz, msz, fl, al = struct.unpack('>8I', core[phoff+32*i:phoff+32*i+32])
    if t == 1 and va <= mark < va + fsz:
        val, = struct.unpack('>I', core[off + mark - va:off + mark - va + 4])
    if t == 4:
        p = off
        while p < off + fsz:
            nsz, dsz, typ = struct.unpack('>3I', core[p:p+12])
            d = p + 12 + ((nsz + 3) & ~3)
            if typ == 1:
                sig, = struct.unpack('>H', core[d+12:d+14])
                pc, = struct.unpack('>I', core[d+70+72:d+70+76])
            p = d + ((dsz + 3) & ~3)
print('marker=%s sig=%s pc_in_boom=%d' % (hex(val) if val is not None else None, sig,
      1 if pc is not None and boom <= pc < boom + size else 0))
PY
)
echo "  (host: $out)"
echo "$out" | grep -q 'marker=0x5a5a1234'
check "the host finds marker = 0x5a5a1234 in the core, at its address from nm" $?
echo "$out" | grep -q 'sig=11 pc_in_boom=1'
check "  and NT_PRSTATUS says SIGSEGV, with a PC inside boom()" $?
block GDB | grep -q 'Program terminated with signal SIGSEGV'
check "the machine's gdb opens it: 'Program terminated with signal SIGSEGV'" $?
block GDB | grep -qx '\$1 = 0x5a5a1234'
check "  and reads marker out of it" $?
block GDB | grep -qE '^#0 .*boom \(here=12648430\)' && block GDB | grep -qE '^#1 .*main'
check "  and the backtrace: boom(here=0xc0ffee), called from main" $?
! grep -aqE '\*\*\* panic|DOUBLE MMU FAULT|kernel stack overflow' "$CL"
check "no panic, no fault" $?

echo
echo "  passed: $pass"
echo "  failed: $fail"
[ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
[ $fail -eq 0 ]
