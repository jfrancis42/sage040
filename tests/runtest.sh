#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis
# runtest.sh <test-name> - boot one test on the Sage040 and print its output.
#
# The guest ends with STOP, which halts the 68040 but does not make QEMU
# exit, so we watch the serial log for the RESULT: sentinel and shut QEMU
# down as soon as it appears instead of waiting out a timeout.
#
# Two serial lines are provided:
#   serial 0 -> the NS16550A console, captured to <test>.out
#   serial 1 -> the MC68901 USART, captured to <test>.usart, and fed from
#               <test>.usartin so the receiver can be tested for real.
set -u
cd "$(dirname "$0")"
t="$1"
SAGE_QEMU="$HOME/m68k/sage040-qemu/bin/qemu-system-m68k"
[ -x "$SAGE_QEMU" ] || SAGE_QEMU=qemu-system-m68k
QEMU="${QEMU:-$SAGE_QEMU}"
out="$t.out"
rm -f "$out" "$t.usart"; : > "$out"

# Known bytes for the MFP USART receiver to pick up.
printf 'RX!' > "$t.usartin"

"$QEMU" -M sage040 -cpu m68040 -m 4 \
    -kernel "$t.elf" \
    -serial "file:$out" \
    -chardev "file,id=mfpusart,path=$t.usart,input-path=$t.usartin" \
    -serial chardev:mfpusart \
    -display none -no-reboot \
    -drive file=disk.img,format=raw,if=ide \
    -nic user,model=smc91c111 >/dev/null 2>&1 &
pid=$!

for _ in $(seq 1 400); do            # up to ~40 s
    grep -q "RESULT:" "$out" 2>/dev/null && break
    kill -0 $pid 2>/dev/null || break
    sleep 0.1
done
kill $pid 2>/dev/null; wait $pid 2>/dev/null

cat "$out"
grep -q "RESULT: PASS" "$out" 2>/dev/null
