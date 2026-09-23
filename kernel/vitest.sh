#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
#
# vitest.sh - vi, the other editor, on this machine.
#
# neatvi (ports/vi), built against picolibc, edits a file the host put
# on the disk, by keystrokes down the serial line: a line opened below
# the last, a word inserted mid-line, a line deleted and the deletion
# undone, an ex substitution across the buffer, :wq. The host reads the
# file back from the image, and vcsnap's copy of /dev/vcsa shows what vi
# drew while it was up.
#
# Runs on a scratch image.

set -u

cd "$(dirname "$0")"

. ../machine.conf

M68K_PREFIX=${M68K_PREFIX:-$HOME/m68k/install}
SAGE_QEMU=${SAGE_QEMU:-$HOME/m68k/sage040-qemu}
QEMU=${QEMU:-$SAGE_QEMU/bin/qemu-system-m68k}
[ -x "$QEMU" ] || QEMU=qemu-system-m68k

SCRATCH=${SAGE_SCRATCH:-$(cd .. && pwd)/scratch}
mkdir -p "$SCRATCH"
DISK="$SCRATCH/hd-vi.img"
PART_LBA=2048
OFFSET=$((PART_LBA * 512))
# The host's end of the disk: one helper, shared with the Makefiles.
# Everything that reaches into the image goes through it, so no test
# carries its own spelling of where the filesystem starts.
FSIMG_SH="$(cd .. && pwd)/tools/fsimg.sh"
fsimg() { PART_OFFSET=$OFFSET "$FSIMG_SH" "$DISK" "$@"; }
LOG="$SCRATCH/vitest.log"
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

echo "=== building ==="
make -s kernel.rom || exit 1
make -s -C ../bootrom bootrom.elf || exit 1
make -s -C ../apps || exit 1
make -s -C ../system || exit 1
make -s -C ../ldso || exit 1
if ! ../ports/vi/build.sh >/dev/null; then
    echo "vitest.sh: could not build vi -- is picolibc built (make libc)?" >&2
    exit 1
fi

echo "=== preparing $DISK ==="
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=16 status=none
printf 'label: dos\nunit: sectors\nstart=%s, type=83\n' "$PART_LBA" \
    | sfdisk -q "$DISK" >/dev/null
fsimg mkfs SAGE040
fsimg put kernel.rom /KERNEL.ROM
fsimg mkdir /bin; fsimg mkdir /lib
# The editor is linked against libc.so: its interpreter and its library.
fsimg put ../ldso/ld.so /lib/ld.so
fsimg put "${SAGE_LIBC:-$HOME/m68k/sage040-libc}/lib/libc.so" /lib/libc.so
fsimg put -m 755 ../ports/vi/vi /bin/vi
fsimg put -m 755 ../apps/vcsnap /bin/vcsnap
fsimg put -m 755 ../system/stty /bin/stty
printf 'the first line\nthe second line\n' > "$SCRATCH/edit.tmp"
LC_ALL=C.UTF-8 fsimg put "$SCRATCH/edit.tmp" "/Notes To Edit.txt"

rm -f "$SCRATCH/in.fifo"
mkfifo "$SCRATCH/in.fifo"
"$QEMU" -M sage040 -cpu m68040 -m "$RAM_MB" \
    -kernel ../bootrom/bootrom.elf \
    -drive file="$DISK",format=raw,if=ide \
    -display none -no-reboot \
    -monitor "unix:$SCRATCH/vi-mon.sock,server,nowait" \
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

# The snapshot is taken 14 s from now: after the editing below (about
# 10.6 s in), before the :wq (about 15.6 s).
printf 'vcsnap 14000 /SNAP.BIN &\r' >&3
sleep 1
printf 'vi "Notes To Edit.txt"\r' >&3
sleep 4
send() { printf '%b' "$1" >&3; sleep 0.4; }
send 'G'; send 'o'; send 'typed in vi'; send '\033'  # a line below the last
send '1G'; send '0'; send '4l'; send 'i'; send 'very '; send '\033'
send '2G'; send 'dd'; send 'u'                      # delete, and undo it
send ':%s/second/2nd/\r'                            # ex, across the buffer
sleep 3
# A picture of it, for the documentation: scratch/vi.png.
printf 'screendump %s\n' "$SCRATCH/vi.ppm" |
    socat - "unix:$SCRATCH/vi-mon.sock" >/dev/null 2>&1
sleep 2
send ':wq\r'
sleep 2
printf '\r' >&3; sleep 0.5
printf 'echo VI-DONE\r' >&3
wait_for "VI-DONE"
sleep 0.5
printf 'stty\r' >&3
sleep 1
printf 'sync\r' >&3
sleep 1

exec 3>&-
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
rm -f "$SCRATCH/in.fifo"

tr -d '\r' < "$LOG" > "$SCRATCH/clean.tmp"
LC_ALL=C.UTF-8 fsimg cat "/Notes To Edit.txt" 2>/dev/null > "$SCRATCH/edited.tmp"
fsimg cat /SNAP.BIN > "$SCRATCH/snap.tmp" 2>/dev/null

echo "=== guest session (escape sequences shown as ^[) ==="
sed 's/\x1b/^[/g; s/^/  | /' "$SCRATCH/clean.tmp" | cut -c1-160 | tail -40

echo "=== checks: the file ==="
echo "  saved file:"; sed 's/^/  | /' "$SCRATCH/edited.tmp"

test "$(sed -n 1p "$SCRATCH/edited.tmp")" = "the very first line"
check "i inserted a word mid-line, after 1G, 0 and 4l" $?
test "$(sed -n 2p "$SCRATCH/edited.tmp")" = "the 2nd line"
check "dd then u put the line back, and :%s changed it" $?
test "$(sed -n 3p "$SCRATCH/edited.tmp")" = "typed in vi"
check "G then o opened a line below the last" $?
test "$(wc -l < "$SCRATCH/edited.tmp")" -eq 3
check "  and nothing else" $?

echo "=== checks: the screen, while it was up ==="
python3 - "$SCRATCH/snap.tmp" > "$SCRATCH/screen.tmp" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
if len(d) < 4:
    sys.exit(0)
rows, cols = d[0], d[1]
for r in range(rows):
    line = "".join(chr(d[4 + (r * cols + c) * 2]) for c in range(cols))
    attr = d[5 + (r * cols) * 2]
    rev = "R" if (attr & 0x07) < ((attr >> 4) & 0x07) else "-"
    print(rev, line.rstrip())
PY
sed 's/^/  | /' "$SCRATCH/screen.tmp"

grep -q "^. the very first line$" "$SCRATCH/screen.tmp"
check "the edited first line was on the screen" $?
grep -q "^. the 2nd line$" "$SCRATCH/screen.tmp" &&
    grep -q "^. typed in vi$" "$SCRATCH/screen.tmp"
check "and the substituted line and the typed one" $?
grep -q "^. ~" "$SCRATCH/screen.tmp"
check "the lines past the end of the file were vi's tildes" $?

echo "=== checks: afterwards ==="
grep -qx "VI-DONE" "$SCRATCH/clean.tmp"
check "vi exited and the shell came back" $?
grep -q "^icrnl opost onlcr isig icanon echo$" "$SCRATCH/clean.tmp"
check "  with the terminal's modes put back as they were" $?

sleep 0.2
magick "$SCRATCH/vi.ppm" "$SCRATCH/vi.png" 2>/dev/null
rm -f "$SCRATCH/edit.tmp" "$SCRATCH/edited.tmp" "$SCRATCH/snap.tmp" "$SCRATCH/screen.tmp" \
      "$SCRATCH/vi.ppm" "$SCRATCH/vi-mon.sock"

echo
echo "  passed: $pass"
echo "  failed: $fail"
if [ "$fail" -eq 0 ]; then echo "RESULT: PASS"; exit 0; fi
echo "RESULT: FAIL"
exit 1
