#!/usr/bin/env bash
# Build and boot the bare-metal 68030 test kernel on stock QEMU.
set -e
cd "$(dirname "$0")"
vasmm68k_mot -m68030 -m68882 -Fbin -o kernel.bin kernel.s
python3 mkelf.py kernel.bin kernel.elf 0x1000
rm -f out.txt
timeout 10 qemu-system-m68k -M virt -cpu "${1:-m68030}" -m 4 \
    -kernel kernel.elf -serial file:out.txt -display none -no-reboot \
    >/dev/null 2>&1 || true
echo "--- serial output ---"; cat out.txt
