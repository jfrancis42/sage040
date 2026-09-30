#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# gfxtest.sh - the graphics demos run, draw, move, and stop cleanly.
#
# Each is started from the serial shell, left for a few seconds, and two
# screendumps are taken a second apart; then q is sent and the harness
# waits for its exit status. Checked: it exits 0 and prints its summary;
# the picture is not blank or a single colour; and for everything that
# animates, the second screendump differs from the first.
#
# mandel is checked for being RIGHT as well: the centre of its starting
# view (-0.6, 0) is inside the set and must be black, and the fixed-point
# and FPU pictures must agree on which pixels are in the set -- two
# implementations that share no arithmetic.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"
. ../machine.conf

SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-/tmp/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-gfx.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/gfxtest.log"
MON="$SCRATCH/gfxtest.mon"
FIFO="$SCRATCH/gfx.fifo"
SHOTS="$SCRATCH/gfxshots"
rm -f "$LOG"
rm -rf "$SHOTS"
mkdir -p "$SHOTS"
BOOT_WAIT=${BOOT_WAIT:-4}

# name | command | seconds before the first screendump | animated? |
# fewest pixels lit (a starfield is mostly black)
DEMOS=(
    "plasma|/bin/plasma|3|1|2000"
    "tunnel|/bin/plasma -k tunnel|4|1|2000"
    "fire|/bin/fire|3|1|2000"
    "mandel|/bin/mandel|10|0|2000"
    "mandelf|/bin/mandel -f|10|0|2000"
    "julia|/bin/mandel -j -f|10|0|2000"
    "voxel|/bin/voxel|4|1|2000"
    "brain|/bin/automata -k brain|3|1|2000"
    "cyclic|/bin/automata -k cyclic|4|1|2000"
    "wireworld|/bin/automata -k wireworld|3|1|2000"
    "ant|/bin/automata -k ant -a 3|3|1|2000"
    "turmite|/bin/automata -k turmite|3|1|2000"
    "sandpile|/bin/sandpile|3|1|2000"
    "dla|/bin/dla|4|1|2000"
    "stars|/bin/stars|3|1|300"
    "balls|/bin/balls|3|1|2000"
    "sorts|/bin/sorts -S 40|6|1|2000"
    "rd|/bin/rd -n 4|4|1|2000"
)

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
make -s -C ../apps || exit 1

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin
for p in plasma fire mandel voxel automata sandpile dla stars balls sorts rd; do
    fsimg put -m 755 ../apps/$p /bin/$p
done

rm -f "$FIFO" "$MON"
mkfifo "$FIFO"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -monitor "unix:$MON,server,nowait" \
    -chardev stdio,id=con,signal=off -serial chardev:con \
    < "$FIFO" > "$LOG" 2>&1 &
qemu_pid=$!
exec 3> "$FIFO"

wait_for() {
    for _ in $(seq 1 "${2:-600}"); do
        [ "$(tr -d '\r' < "$LOG" | grep -cxE -- "$1")" -ge 1 ] && return 0
        kill -0 "$qemu_pid" 2>/dev/null || return 1
        sleep 0.2
    done
    return 1
}
shot() {
    printf 'screendump %s\n' "$1" | socat - "unix:$MON" >/dev/null
    for _ in $(seq 1 50); do
        [ -s "$1" ] && return 0
        sleep 0.1
    done
}

sleep "$BOOT_WAIT"
for d in "${DEMOS[@]}"; do
    IFS='|' read -r name cmd secs anim minlit <<< "$d"
    # The marker split by quotes, so the echo of the typed line is not
    # already it.
    printf '%s; echo GFX-"%s" $?\r' "$cmd" "$name" >&3
    sleep "$secs"
    shot "$SHOTS/$name-1.ppm"
    sleep 1
    shot "$SHOTS/$name-2.ppm"
    printf 'q' >&3
    wait_for "GFX-$name [0-9]+" 300
done

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$FIFO"
tr -d '\r' < "$LOG" > "$SCRATCH/gfx-clean.tmp"

echo "=== checks ==="
for d in "${DEMOS[@]}"; do
    IFS='|' read -r name cmd secs anim minlit <<< "$d"
    prog=${cmd%% *}
    prog=${prog##*/}
    grep -qxE "GFX-$name 0" "$SCRATCH/gfx-clean.tmp"
    check "$name: exits 0" $?
    out=$(python3 - "$SHOTS/$name-1.ppm" "$SHOTS/$name-2.ppm" <<'PY'
import sys
def load(p):
    d = open(p, 'rb').read()
    parts = d.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    px = parts[4]
    return w, h, [px[i:i + 3] for i in range(0, w * h * 3, 3)]
try:
    w, h, a = load(sys.argv[1])
    _, _, b = load(sys.argv[2])
except Exception:
    print("0 0 0")
    sys.exit()
colours = len(set(a))
lit = sum(1 for p in a if p != b'\x00\x00\x00')
changed = sum(1 for x, y in zip(a, b) if x != y)
print(colours, lit, changed)
PY
)
    read -r colours lit changed <<< "$out"
    [ "${colours:-0}" -ge 3 ] && [ "${lit:-0}" -ge "$minlit" ]
    check "$name: draws a picture ($colours colours, $lit pixels lit)" $?
    if [ "$anim" = 1 ]; then
        [ "${changed:-0}" -ge 100 ]
        check "$name: and it moves ($changed pixels changed in a second)" $?
    fi
done

# The summaries, where there is one to find.
for s in plasma fire voxel automata sandpile dla stars balls rd; do
    grep -qE "^$s: .* in [0-9]+ seconds" "$SCRATCH/gfx-clean.tmp"
    check "$s prints its summary on the way out" $?
done
grep -qE '^sorts: [a-z]+, [0-9]+ bars: [0-9]+ comparisons, [0-9]+ writes, sorted$' \
    "$SCRATCH/gfx-clean.tmp" && ! grep -q 'NOT SORTED' "$SCRATCH/gfx-clean.tmp"
check "sorts: every algorithm it finished left the bars sorted" $?

echo "=== checks: mandel is right ==="
grep -qE '^mandel: Mandelbrot, 256 iterations, fixed point: [0-9]+ ms' \
    "$SCRATCH/gfx-clean.tmp"
check "mandel finished a fixed-point picture and timed it" $?
grep -qE '^mandel: Mandelbrot, 256 iterations, FPU: [0-9]+ ms' \
    "$SCRATCH/gfx-clean.tmp"
check "  and an FPU one" $?
out=$(python3 - "$SHOTS/mandel-1.ppm" "$SHOTS/mandelf-1.ppm" <<'PY'
import sys
def load(p):
    d = open(p, 'rb').read()
    parts = d.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    px = parts[4]
    return w, h, [px[i:i + 3] == b'\x00\x00\x00' for i in range(0, w * h * 3, 3)]
w, h, a = load(sys.argv[1])
_, _, b = load(sys.argv[2])
same = sum(1 for x, y in zip(a, b) if x == y) * 1000 // len(a)
centre = a[(h // 2) * w + w // 2]
left = a[(h // 2) * w + 2]
print(same, int(centre), int(left))
PY
)
read -r same centre left <<< "$out"
[ "${centre:-0}" = 1 ] && [ "${left:-1}" = 0 ]
check "the centre (-0.6, 0) is in the set, black; the far left is not" $?
[ "${same:-0}" -ge 995 ]
check "fixed point and the FPU agree on the set, pixel for pixel ($same per mille)" $?

grep -qE 'panic|bus error|address error' "$SCRATCH/gfx-clean.tmp"
[ $? -ne 0 ]
check "no panic, no fault" $?

echo
echo "--- $pass passed, $fail failed ---"
if [ "$fail" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
