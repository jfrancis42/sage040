#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
# shot.sh - run the cube headless and capture frames as PNGs.
#
# QEMU is driven through its monitor socket: let the cube tumble for a
# moment, then ask for a screendump.  Repeat to catch several angles.
set -eu
cd "$(dirname "$0")"
SAGE_QEMU="$HOME/m68k/sage040-qemu/bin/qemu-system-m68k"
[ -x "$SAGE_QEMU" ] || SAGE_QEMU=qemu-system-m68k
QEMU="${QEMU:-$SAGE_QEMU}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
MON="$TMP/cube-mon.sock"
N="${1:-3}"

mkdir -p docs
rm -f "$MON" cube.out

SPEED="${SPEED:-6}"
[ "$SPEED" = off ] && ICOUNT="" || ICOUNT="-icount shift=$SPEED,sleep=on"

# shellcheck disable=SC2086
"$QEMU" -M sage040 -cpu m68040 -m 4 $ICOUNT -kernel cube.elf \
    -serial file:cube.out -display none \
    -monitor "unix:$MON,server,nowait" &
pid=$!
sleep 2

for i in $(seq 1 "$N"); do
    sleep 1
    printf 'screendump %s/cube%d.ppm\n' "$TMP" "$i" | socat - "unix:$MON" >/dev/null
    sleep 0.3
    magick "$TMP/cube$i.ppm" "docs/cube$i.png" 2>/dev/null || true
done

kill $pid 2>/dev/null || true
wait $pid 2>/dev/null || true

echo "--- serial output ---"
head -12 cube.out
ls -l docs/*.png
