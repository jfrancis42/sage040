#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# nativetest.sh - the toolchain running ON the machine (task 49).
#
# The claim being tested is not "gcc is installed". It is that the
# Sage040 can take C source and produce a program that runs, using
# nothing but itself -- no cross compiler, no other computer.
#
# THE CHECK THAT MATTERS is the last one: the same source is compiled
# HERE by the cross compiler and THERE by the native one, and the two
# object files are compared byte for byte. They are the same gcc
# version with the same flags for the same target, so they should
# agree exactly -- and if they do, the native compiler is not merely
# "a compiler that runs", it is demonstrably the same compiler.
# Anything short of that leaves room for a native gcc that is subtly
# miscompiled and produces subtly wrong code.
#
# The machine needs more of both than usual for this: cc1 is a large
# program with a large working set, and the toolchain is ~100 MB of
# disk. NATIVE_RAM_MB and NATIVE_DISK_MB below are deliberately
# separate from machine.conf's, which describes the ordinary machine.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SRCDIR=${SAGE_SRC:-$HOME/m68k/src}
BINOUT=$SRCDIR/build-binutils-native/sage040
GCCOUT=$SRCDIR/build-gcc-native/sage040
SAGE_LIBC=${SAGE_LIBC:-$HOME/m68k/sage040-libc}

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-native.img"
PART_LBA=2048
MIMG="$DISK@@$((PART_LBA * 512))"
LOG="$SCRATCH/nativetest.log"
WORK="$SCRATCH/native.tmp"
rm -f "$LOG"; rm -rf "$WORK"; mkdir -p "$WORK"

NATIVE_RAM_MB=${NATIVE_RAM_MB:-256}
NATIVE_DISK_MB=${NATIVE_DISK_MB:-512}
BOOT_WAIT=${BOOT_WAIT:-5}

pass=0
fail=0
check() {
    if [ "$2" -eq 0 ]; then
        echo "  [ OK ] $1"; pass=$((pass + 1))
    else
        echo "  [FAIL] $1"; fail=$((fail + 1))
    fi
}
skip() { echo "  [SKIP] $1"; }

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../system sh || exit 1
make -s -C ../ldso || exit 1
[ -x "$BINOUT/bin/as" ] || ../ports/binutils/build.sh > /dev/null || exit 1
if [ ! -x "$GCCOUT/bin/gcc" ]; then
    echo "nativetest: no native gcc in $GCCOUT -- run ports/gcc/build.sh" >&2
    exit 1
fi

# The programs to compile. Small on purpose: this is about whether the
# toolchain works, not how fast it is, and a 25 MHz 68040 compiling
# under emulation is not quick.
cat > "$WORK/hello.c" <<'EOF'
#include <stdio.h>
int main(int argc, char **argv)
{
    printf("hello from %s, argc=%d\n", argv[0], argc);
    return 0;
}
EOF
# Something with arithmetic the compiler must get right, and a loop it
# will optimise: a program that prints a constant proves less.
cat > "$WORK/maths.c" <<'EOF'
#include <stdio.h>
static unsigned long long fib(int n)
{
    unsigned long long a = 0, b = 1, t;
    while (n-- > 0) { t = a + b; a = b; b = t; }
    return a;
}
int main(void)
{
    double x = 1.0;
    int i;
    for (i = 1; i <= 10; i++) { x = x + 1.0 / (double)i; }
    printf("fib(90)=%llu\n", fib(90));
    printf("harmonic=%.6f\n", x);
    printf("sizes=%d %d %d %d\n", (int)sizeof(char), (int)sizeof(short),
           (int)sizeof(int), (int)sizeof(long));
    return 0;
}
EOF
# Two files, so the machine's own `ld` has to put them together.
cat > "$WORK/part1.c" <<'EOF'
extern int twice(int);
#include <stdio.h>
int main(void) { printf("twice(21)=%d\n", twice(21)); return 0; }
EOF
cat > "$WORK/part2.c" <<'EOF'
int twice(int x) { return x * 2; }
EOF

echo "=== what the CROSS compiler makes of the same source ==="
. ../ports/cross.sh
# shellcheck disable=SC2086
for f in hello maths part2; do
    "$CROSS_CC" $CROSS_CFLAGS $CROSS_CPPFLAGS -c "$WORK/$f.c" -o "$WORK/$f.host.o" \
        2>/dev/null || echo "    (cross compile of $f failed)"
done

echo "=== preparing $DISK ($NATIVE_DISK_MB MB, $NATIVE_RAM_MB MB RAM) ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=$NATIVE_DISK_MB status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=06\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
mkfs.fat -F 16 -n SAGE040 --offset "$PART_LBA" "$DISK" \
    $(( (NATIVE_DISK_MB * 2048 - PART_LBA) / 2 )) >/dev/null
mcopy -o -i "$MIMG" kernel.rom ::/KERNEL.ROM
mmd -i "$MIMG" ::/BIN ::/lib ::/usr ::/usr/bin ::/usr/lib ::/usr/include ::/ST
mcopy -o -i "$MIMG" ../system/sh ::/BIN/sh
mcopy -o -i "$MIMG" ../ldso/ld.so ::/lib/ld.so
mcopy -o -i "$MIMG" "$SAGE_LIBC/lib/libc.so" ::/lib/libc.so
for p in ls cat echo cmp od wc; do
    [ -x "../ports/sbase/bin/$p" ] && \
        mcopy -o -i "$MIMG" "../ports/sbase/bin/$p" "::/BIN/$p"
done

echo "    copying the toolchain onto the disk (this takes a minute)"
for b in "$BINOUT"/bin/*; do
    mcopy -o -i "$MIMG" "$b" "::/usr/bin/$(basename "$b")"
done
# gcc: the driver, and libexec's cc1, which is where the compiler is.
( cd "$GCCOUT" && find . -type f > "$WORK/gccfiles.txt" )
while read -r f; do
    d=$(dirname "$f")
    [ "$d" = "." ] || mmd -D s -i "$MIMG" "::/usr/${d#./}" 2>/dev/null || true
done < <(awk -F/ '{for(i=2;i<NF;i++){printf "%s%s", (i==2?"./":"/"), $i}; print ""}' \
         "$WORK/gccfiles.txt" | sort -u)
( cd "$GCCOUT" && tar cf - . ) | ( cd "$WORK" && rm -rf gccstage && mkdir gccstage && tar xf - -C gccstage )
mcopy -s -o -i "$MIMG" "$WORK/gccstage"/* ::/usr/ 2>/dev/null || true

# The C library on the machine, which is what a native link needs.
mcopy -s -o -i "$MIMG" "$SAGE_LIBC/include"/* ::/usr/include/ 2>/dev/null
for f in libc.a libc.so liblinux.a libm.a; do
    mcopy -o -i "$MIMG" "$SAGE_LIBC/lib/$f" ::/usr/lib/
done
mcopy -o -i "$MIMG" "$("$CROSS_CC" -mcpu=68040 -print-libgcc-file-name)" ::/usr/lib/
mcopy -o -i "$MIMG" ../libc/crt0.o ../libc/crt0-dyn.o ::/usr/lib/
mcopy -o -i "$MIMG" ../libc/sage040.ld ::/usr/lib/
mcopy -o -i "$MIMG" ../libc/sage040.specs ::/usr/lib/
for f in hello maths part1 part2; do
    mcopy -o -i "$MIMG" "$WORK/$f.c" "::/ST/$f.c"
done

rm -f "$SCRATCH/native.fifo"; mkfifo "$SCRATCH/native.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$NATIVE_RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$SCRATCH/native.fifo" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$SCRATCH/native.fifo"

wait_for() {
    for _ in $(seq 1 "${2:-9000}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxF -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
run() {
    printf '%s\r' "$1" >&3
    printf 'echo DONE-%s\r' "$2" >&3
    wait_for "DONE-$2" "${3:-9000}" || echo "    (timed out: $2)"
    sleep 0.1
}

sleep "$BOOT_WAIT"
run 'cd /ST' cd
run 'gcc --version > /ST/ver.out 2>&1'                       ver
run 'as --version > /ST/asver.out 2>&1'                      asver
echo "    compiling hello.c on the machine..."
run 'gcc -specs=/usr/lib/sage040.specs hello.c -o hello > /ST/c1.out 2>&1' c1
run './hello > /ST/run1.out 2>&1'                            r1
echo "    compiling maths.c ..."
run 'gcc -specs=/usr/lib/sage040.specs -O2 maths.c -o maths > /ST/c2.out 2>&1' c2
run './maths > /ST/run2.out 2>&1'                            r2
echo "    two files, linked by the machine's own ld ..."
run 'gcc -specs=/usr/lib/sage040.specs part1.c part2.c -o parts > /ST/c3.out 2>&1' c3
run './parts > /ST/run3.out 2>&1'                            r3
echo "    -c only, for the byte-for-byte comparison ..."
run 'gcc -mcpu=68040 -O2 -c hello.c -o hello.native.o > /ST/c4.out 2>&1' c4
run 'gcc -mcpu=68040 -O2 -c maths.c -o maths.native.o > /ST/c5.out 2>&1' c5
run 'ls -l /ST > /ST/ls.out 2>&1'                            ls
run 'echo ALL-DONE'                                          end
wait_for "ALL-DONE"
sleep 1

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/native.fifo"
tr -d '\r' < "$LOG" > "$WORK/session.txt"

get() { mcopy -n -o -i "$MIMG" "::/ST/$1" "$WORK/$1" 2>/dev/null; }
for f in ver.out asver.out c1.out c2.out c3.out c4.out c5.out \
         run1.out run2.out run3.out ls.out \
         hello maths parts hello.native.o maths.native.o; do
    get "$f"
done

echo "=== session (tail) ==="
sed 's/^/  | /' "$WORK/session.txt" | tail -20

echo "=== checks ==="

! grep -qE "panic|exception at|DOUBLE MMU" "$WORK/session.txt"
check "no panic, no kernel exception" $?

grep -q "15.2.0" "$WORK/ver.out" 2>/dev/null
check "gcc runs on the machine and reports 15.2.0" $?

grep -qi "GNU assembler" "$WORK/asver.out" 2>/dev/null
check "  and so does the assembler" $?

test -s "$WORK/hello"
check "the machine compiled and linked hello.c" $?

grep -q "hello from" "$WORK/run1.out" 2>/dev/null
check "  and the program it produced RUNS" $?

grep -q "fib(90)=2880067194370816120" "$WORK/run2.out" 2>/dev/null
check "64-bit arithmetic in a natively compiled program is right" $?

grep -q "harmonic=2.928968" "$WORK/run2.out" 2>/dev/null
check "  and so is the floating point" $?

grep -q "sizes=1 2 4 4" "$WORK/run2.out" 2>/dev/null
check "  and the type sizes are this target's" $?

grep -q "twice(21)=42" "$WORK/run3.out" 2>/dev/null
check "two source files, linked by the machine's own ld, run" $?

# --- the one that matters --------------------------------------------
#
# Same source, same flags, same compiler version, one run here and one
# run there. If the object files differ, the two compilers are not the
# same compiler, whatever their --version says.
for f in hello maths; do
    if [ -s "$WORK/$f.native.o" ] && [ -s "$WORK/$f.host.o" ]; then
        cmp -s "$WORK/$f.native.o" "$WORK/$f.host.o"
        check "$f.o is byte-for-byte what the CROSS compiler produces" $?
    else
        check "$f.o is byte-for-byte what the CROSS compiler produces" 1
    fi
done

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
