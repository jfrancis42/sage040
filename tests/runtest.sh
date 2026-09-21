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

# QEMU refuses to start at all if the drive file is missing, and then
# writes nothing, which looks exactly like a guest that crashed before its
# first character.  Create it here rather than relying on the Makefile, so
# running this script by hand behaves the same way.
[ -f disk.img ] || dd if=/dev/zero of=disk.img bs=1M count=8 status=none

# A signature written by the host, so t3-ata can prove it reads the media
# byte for byte rather than merely round-tripping its own writes.
if [ -f disk.img ]; then
    printf 'SAGE040-DISK-OK!' \
        | dd of=disk.img bs=512 seek=2 conv=notrunc status=none 2>/dev/null || true
fi

# A monitor socket, so a test that needs keystrokes can be typed at.
# QEMU delivers no keyboard input with -display none -- there is no
# window to take focus -- so `sendkey` on the monitor is the only way a
# test can exercise the 8042 at all.
mon="$t.mon"
rm -f "$mon"

"$QEMU" -M sage040 -cpu m68040 -m 4 \
    -kernel "$t.elf" \
    -monitor "unix:$mon,server,nowait" \
    -serial "file:$out" \
    -chardev "file,id=mfpusart,path=$t.usart,input-path=$t.usartin" \
    -serial chardev:mfpusart \
    -display none -no-reboot \
    -drive file=disk.img,format=raw,if=ide \
    -nic user,model=smc91c111 >/dev/null 2>&1 &
pid=$!

# Keys a test asks for. It prints KEYS-PLEASE when it is listening, and
# the file next to it says what to send; without the handshake the codes
# would arrive before anything was reading them.
keys=""
[ -f "$t.keys" ] && keys=$(cat "$t.keys")
keys_sent=0

for _ in $(seq 1 400); do            # up to ~40 s
    if [ -n "$keys" ] && [ "$keys_sent" -eq 0 ] &&
       grep -q "KEYS-PLEASE" "$out" 2>/dev/null; then
        for k in $keys; do
            echo "sendkey $k" | socat - "unix:$mon" >/dev/null 2>&1
            sleep 0.1
        done
        keys_sent=1
    fi
    grep -q "RESULT:" "$out" 2>/dev/null && break
    kill -0 $pid 2>/dev/null || break
    sleep 0.1
done
kill $pid 2>/dev/null; wait $pid 2>/dev/null
rm -f "$mon"

cat "$out"
grep -q "RESULT: PASS" "$out" 2>/dev/null
